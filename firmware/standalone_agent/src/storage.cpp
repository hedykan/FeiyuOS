#include "storage.h"

#include "ops_lora.h"

#include <LittleFS.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>

#include <atomic>
#include <map>
#include <functional>
#include <set>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace vat::storage {
namespace {

constexpr int kSdSck = 40;
constexpr int kSdMiso = 39;
constexpr int kSdMosi = 14;
constexpr int kSdCs = 12;
// FATFS reserves a full sector buffer (~4 KB) per file slot at mount time.
// The firmware never holds more than three files open at once (an append
// from the UI task, one from the worker, one read), so three slots: every
// slot dropped is ~4 KB more heap for TLS.
constexpr uint8_t kSdMaxFiles = 3;
// LittleFS is ~1.5 MB: keep logs small enough that a full recording still fits.
constexpr std::size_t kRotateBytesTf = 192 * 1024;
constexpr std::size_t kRotateBytesFlash = 48 * 1024;

fs::FS* active = nullptr;
SemaphoreHandle_t append_mutex = xSemaphoreCreateMutex();  // UI + worker both log
String active_kind = "none";

// Copies a file or directory tree; false if any file could not be written whole.
bool copy_tree(fs::FS& from, fs::FS& to, const String& path) {
    File source = from.open(path);
    if (!source) return false;
    if (!source.isDirectory()) {
        File target = to.open(path, FILE_WRITE);
        bool ok = static_cast<bool>(target);
        uint8_t buffer[512];
        int n;
        while (ok && (n = source.read(buffer, sizeof(buffer))) > 0) ok = target.write(buffer, n) == static_cast<size_t>(n);
        if (target) target.close();
        source.close();
        return ok;
    }
    source.close();  // list children first so recursion never holds a directory open
    if (!to.exists(path) && !to.mkdir(path)) return false;
    std::vector<String> children;
    File dir = from.open(path);
    File entry = dir.openNextFile();
    while (entry) {
        children.push_back(entry.path());
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    bool ok = true;
    for (const auto& child : children) {
        if (!child.startsWith("/vat/tests") && !child.startsWith("/vat/cache")) ok = copy_tree(from, to, child) && ok;
    }
    return ok;
}

// Folds a degraded flash session into the card without overwriting it: files
// missing on the card are copied, append-only logs (*.jsonl) are appended,
// everything else on the card is kept.
bool merge_tree(fs::FS& from, fs::FS& to, const String& path) {
    File source = from.open(path);
    if (!source) return false;
    if (!source.isDirectory()) {
        const bool exists = to.exists(path);
        if (exists && !path.endsWith(".jsonl")) {
            source.close();
            return true;
        }
        File target = to.open(path, exists ? FILE_APPEND : FILE_WRITE);
        bool ok = static_cast<bool>(target);
        uint8_t buffer[512];
        int n;
        while (ok && (n = source.read(buffer, sizeof(buffer))) > 0) ok = target.write(buffer, n) == static_cast<size_t>(n);
        if (target) target.close();
        source.close();
        return ok;
    }
    source.close();
    if (!to.exists(path) && !to.mkdir(path)) return false;
    std::vector<String> children;
    File dir = from.open(path);
    File entry = dir.openNextFile();
    while (entry) {
        children.push_back(entry.path());
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    bool ok = true;
    for (const auto& child : children) {
        if (!child.startsWith("/vat/tests") && !child.startsWith("/vat/cache")) ok = merge_tree(from, to, child) && ok;
    }
    return ok;
}

void remove_tree_on(fs::FS& fs, const String& path) {
    File node = fs.open(path);
    if (!node) return;
    if (!node.isDirectory()) {
        node.close();
        fs.remove(path);
        return;
    }
    std::vector<String> children;
    File entry = node.openNextFile();
    while (entry) {
        children.push_back(entry.path());
        entry.close();
        entry = node.openNextFile();
    }
    node.close();
    for (const auto& child : children) remove_tree_on(fs, child);
    fs.rmdir(path);
}


// ---- slow card operations --------------------------------------------------
// The SD library busy-waits while the card is slow (500 ms x 3 per sector).
// Every storage call longer than a second is counted and printed on the
// serial console (not to the runtime log: that is itself on the card).
std::atomic<uint32_t> slow_count{0};
std::atomic<uint32_t> slow_max_ms{0};

struct Timed {
    const char* op;
    const String& path;
    unsigned long started = millis();
    Timed(const char* op, const String& path) : op(op), path(path) {}
    ~Timed() {
        const uint32_t ms = millis() - started;
        if (ms < 1000) return;
        ++slow_count;
        if (ms > slow_max_ms) slow_max_ms = ms;
        Serial.printf("[sd-slow] %s %s %lums core%d\n", op, path.c_str(), static_cast<unsigned long>(ms), xPortGetCoreID());
    }
};

// ---- file size cache -------------------------------------------------------
// Size of files this firmware wrote or looked at, or -1 when known missing.
// On this card every stat or remove is a directory search that can stall for
// seconds (`remove task.jsonl` and `size summary.json` showed 1-2 s each), so
// answers come from here when they can.  Every write path below keeps it
// current; code writing through fs() directly calls changed().  Cleared when
// the card is remounted.
constexpr long kUnknown = -2;
std::map<String, long> sizes;
SemaphoreHandle_t sizes_mutex = xSemaphoreCreateMutex();

long cached_size(const String& path) {
    xSemaphoreTake(sizes_mutex, portMAX_DELAY);
    const auto it = sizes.find(path);
    const long value = it == sizes.end() ? kUnknown : it->second;
    xSemaphoreGive(sizes_mutex);
    return value;
}

void remember(const String& path, long bytes) {
    xSemaphoreTake(sizes_mutex, portMAX_DELAY);
    if (sizes.size() > 200) sizes.clear();  // bounded; it is only a cache
    if (bytes == kUnknown) sizes.erase(path);
    else sizes[path] = bytes;
    xSemaphoreGive(sizes_mutex);
}

// After appending `added` bytes.  File::size() cannot be used for this: on
// the SD card it stats the directory entry, which still holds the size from
// before the unflushed write (it once cut the last tool result out of a
// request and DeepSeek rejected every call with HTTP 400).
void grew(const String& path, long before, std::size_t added) {
    remember(path, before >= 0 ? before + static_cast<long>(added) : before == -1 ? static_cast<long>(added) : kUnknown);
}

void forget_under(const String& prefix) {
    xSemaphoreTake(sizes_mutex, portMAX_DELAY);
    for (auto it = sizes.begin(); it != sizes.end();) {
        if (it->first == prefix || it->first.startsWith(prefix + "/")) it = sizes.erase(it);
        else ++it;
    }
    xSemaphoreGive(sizes_mutex);
}

// ---- card mount --------------------------------------------------------------
// 10 MHz, then 4 MHz.  The official example uses 25 MHz, but the established
// Cardputer firmwares run the card at 4 MHz day to day (Bruce, Launcher; the
// latter reaches ~277 KB/s on the Adv at 4 MHz) and raise the clock only for
// bulk transfers after a CRC check.  25 MHz here gave 0.5-3.6 s stalls and a
// card that dropped out after ~20 minutes of heavy use.
constexpr uint32_t kSdFrequencies[] = {10000000, 4000000};
uint32_t sd_hz = 0;

bool mount_card() {
    for (const auto frequency : kSdFrequencies) {
        SD.end();
        SPI.end();
        SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
        delay(20);
        if (SD.begin(kSdCs, SPI, frequency, "/sd", kSdMaxFiles, false) && SD.cardType() != CARD_NONE) {
            sd_hz = frequency;
            return true;
        }
        delay(50);
    }
    return false;
}

// ---- flash mirror ----------------------------------------------------------
// What the device needs to boot, get online and reach its hosts is mirrored
// to /mirror on the on-board flash, so a failed card costs the long context,
// not the device: Wi-Fi and keys, settings, SSH hosts and the device key, the
// user's notes, device records.  Bulky, write-heavy data stays card-only.
constexpr char kMirrorRev[] = "/mirror/rev";
constexpr char kCardRev[] = "/vat/.mirror_rev";
constexpr char kDegradedMark[] = "/vat/.degraded";
constexpr std::size_t kMirroredNotesBytes = 4096;
const char* const kMirroredFiles[] = {"/vat/config.json", "/vat/secrets.json", "/vat/ssh/profiles.json",
                                      "/vat/ssh/id_ed25519", "/vat/memory/notes.txt"};
bool flash_mounted = false;
bool degraded_mode = false;

bool is_mirrored(const String& path) {
    for (const char* file : kMirroredFiles) {
        if (path == file) return true;
    }
    return path.startsWith("/vat/things/") && !path.endsWith(".jsonl");
}

String mirror_of(const String& path) { return "/mirror" + path.substring(4); }  // "/vat/x" -> "/mirror/x"
String vat_of(const String& path) { return "/vat" + path.substring(7); }        // "/mirror/x" -> "/vat/x"

bool make_dirs(fs::FS& fs, const String& path) {
    int slash = path.indexOf('/', 1);
    while (slash > 0) {
        const String directory = path.substring(0, slash);
        if (!fs.exists(directory) && !fs.mkdir(directory)) return false;
        slash = path.indexOf('/', slash + 1);
    }
    return true;
}

// Copies one file (its last `tail` bytes when non-zero) across filesystems.
bool copy_file(fs::FS& from, const String& source, fs::FS& to, const String& target, std::size_t tail = 0) {
    File in = from.open(source, FILE_READ);
    if (!in || in.isDirectory()) return false;
    if (tail && in.size() > tail) in.seek(in.size() - tail);
    if (!make_dirs(to, target)) return false;
    const String temp = target + ".tmp";
    to.remove(temp);
    File out = to.open(temp, FILE_WRITE);
    bool ok = static_cast<bool>(out);
    uint8_t buffer[256];
    int n;
    while (ok && (n = in.read(buffer, sizeof(buffer))) > 0) ok = out.write(buffer, n) == static_cast<size_t>(n);
    in.close();
    if (out) out.close();
    if (!ok) {
        to.remove(temp);
        return false;
    }
    to.remove(target);
    return to.rename(temp, target);
}

long read_rev(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) return 0;
    const long rev = file.readString().toInt();
    file.close();
    return rev;
}

void write_rev(fs::FS& fs, const char* path, long rev) {
    make_dirs(fs, path);
    File file = fs.open(path, FILE_WRITE);
    if (file) {
        file.print(rev);
        file.close();
    }
}

// Every mirrored file present under `root` ("/vat" or "/mirror") of `fs`.
std::vector<String> mirrored_present(fs::FS& fs, const String& root) {
    std::vector<String> found;
    for (const char* file : kMirroredFiles) {
        const String path = root + String(file).substring(4);
        if (fs.exists(path)) found.push_back(path);
    }
    std::vector<String> pending{root + "/things"};
    while (!pending.empty()) {
        const String directory = pending.back();
        pending.pop_back();
        File dir = fs.open(directory);
        if (!dir || !dir.isDirectory()) continue;
        std::vector<String> children;
        File entry = dir.openNextFile();
        while (entry) {
            children.push_back(entry.path());
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
        for (const auto& child : children) {
            File node = fs.open(child);
            const bool is_dir = node && node.isDirectory();
            if (node) node.close();
            if (is_dir) pending.push_back(child);
            else if (!child.endsWith(".jsonl") && !child.endsWith(".tmp")) found.push_back(child);
        }
    }
    return found;
}

// One mirrored file changed on the active store: refresh its copy and bump
// the revision on both sides (the newer side wins when the card comes back).
void mirror_file(const String& path) {
    if (!flash_mounted || !active || !is_mirrored(path)) return;
    const String copy = mirror_of(path);
    if (active->exists(path)) {
        copy_file(*active, path, LittleFS, copy, path.endsWith("notes.txt") ? kMirroredNotesBytes : 0);
    } else {
        LittleFS.remove(copy);
    }
    const long rev = std::max(read_rev(LittleFS, kMirrorRev), read_rev(*active, kCardRev)) + 1;
    write_rev(LittleFS, kMirrorRev, rev);
    write_rev(*active, kCardRev, rev);
}

// Card mounted: the newer side of card and mirror wins.
void sync_mirror_with_card() {
    const long card = read_rev(SD, kCardRev);
    const long flash = read_rev(LittleFS, kMirrorRev);
    if (flash > card) {  // changed while the card was unavailable
        for (const auto& path : mirrored_present(LittleFS, "/mirror")) copy_file(LittleFS, path, SD, vat_of(path));
        write_rev(SD, kCardRev, flash);
        return;
    }
    for (const auto& path : mirrored_present(SD, "/vat")) {
        copy_file(SD, path, LittleFS, mirror_of(path), path.endsWith("notes.txt") ? kMirroredNotesBytes : 0);
    }
    const long rev = std::max(card, 1L);
    write_rev(LittleFS, kMirrorRev, rev);
    write_rev(SD, kCardRev, rev);
}

// No card: boot from the mirror when flash holds no configuration of its own.
void restore_from_mirror() {
    if (LittleFS.exists(kDegradedMark)) {
        degraded_mode = true;
        return;
    }
    if (LittleFS.exists("/vat/config.json") || LittleFS.exists("/vat/secrets.json")) return;  // real flash-only use
    const auto files = mirrored_present(LittleFS, "/mirror");
    if (files.empty()) return;
    for (const auto& path : files) copy_file(LittleFS, path, LittleFS, vat_of(path));
    write_rev(LittleFS, kCardRev, read_rev(LittleFS, kMirrorRev));
    File mark = LittleFS.open(kDegradedMark, FILE_WRITE);
    if (mark) mark.close();
    degraded_mode = true;
}
void flush_for(const String& path);  // write-behind buffers (defined with the appends)

}  // namespace

bool begin() {
    pinMode(kSdCs, OUTPUT);
    digitalWrite(kSdCs, HIGH);
    if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
        // The Cap LoRa-1262 shares SCK/MOSI/MISO with the TF slot: keep its
        // SX1262 off the bus (chip select high, held in reset) until
        // lora::begin() decides to run it.
        lora::hold_off_bus();
    }
    // After a watchdog or software reset the card was not power-cycled and
    // may still be busy with the write that was interrupted: give it time
    // instead of falling back to flash (which also made the device lose its
    // Wi-Fi settings and open the setup access point).
    const bool cold = esp_reset_reason() == ESP_RST_POWERON;
    const int rounds = cold ? 1 : 5;
    for (int round = 0; round < rounds && !active; ++round) {
        if (round) delay(600);
        if (mount_card()) {
            active = &SD;
            active_kind = "tf";
        }
    }
    if (!active && LittleFS.begin(true)) {
        active = &LittleFS;
        active_kind = "flash";
        flash_mounted = true;
        restore_from_mirror();
    }
    if (!active) return false;
    if (active_kind == "tf" && LittleFS.begin(true)) {
        flash_mounted = true;  // stays mounted for the mirror
        // Data lives in exactly one place.  A flash session that carried a
        // full configuration was real use without a card: it is newer, so it
        // replaces the card's copy.  One without (the card failed to mount
        // after a reset) only adds what is missing and appends its logs; it
        // must not overwrite the card (it once replaced the runtime log that
        // held the cause of the reset).
        // A session restored from the mirror is degraded, never a real one:
        // its mirrored files come back through the mirror revision below.
        if (LittleFS.exists("/vat")) {
            const bool configured = !LittleFS.exists(kDegradedMark) && LittleFS.exists("/vat/config.json") &&
                                    LittleFS.exists("/vat/secrets.json");
            const bool moved = configured ? copy_tree(LittleFS, SD, "/vat") : merge_tree(LittleFS, SD, "/vat");
            if (moved) remove_tree_on(LittleFS, "/vat");
        }
        sync_mirror_with_card();
    }
    for (const char* dir : {"/vat", "/vat/logs", "/vat/memory", "/vat/cache",
                            "/vat/cache/jobs", "/vat/ssh", "/vat/tests"}) {
        if (!active->exists(dir)) active->mkdir(dir);
    }
    return true;
}

bool ready() { return active != nullptr; }
fs::FS& fs() { return *active; }
String kind() { return active_kind; }

uint64_t total_bytes() {
    if (active_kind == "tf") return SD.totalBytes();
    if (active_kind == "flash") return LittleFS.totalBytes();
    return 0;
}

uint64_t used_bytes() {
    if (active_kind == "tf") return SD.usedBytes();
    if (active_kind == "flash") return LittleFS.usedBytes();
    return 0;
}

bool exists(const String& path) {
    const long known = cached_size(path);
    if (known != kUnknown) return known >= 0;
    const Timed timed("exists", path);
    return active && active->exists(path);
}

// Directories known to exist: every append used to stat each level of its
// path, a directory search on the card each time.  Cleared when trees go.
std::set<String> known_dirs;

bool ensure_parent(const String& path) {
    const Timed timed("ensure_parent", path);
    if (!active) return false;
    int slash = path.indexOf('/', 1);
    while (slash > 0) {
        const String directory = path.substring(0, slash);
        if (!known_dirs.count(directory)) {
            if (!active->exists(directory) && !active->mkdir(directory)) return false;
            known_dirs.insert(directory);
        }
        slash = path.indexOf('/', slash + 1);
    }
    return true;
}

String read_text(const String& path, std::size_t max_bytes) {
    flush_for(path);
    const Timed timed("read_text", path);
    if (!active) return {};
    File file = active->open(path, FILE_READ);
    if (!file || file.isDirectory()) return {};
    String content;
    const std::size_t size = std::min<std::size_t>(file.size(), max_bytes);
    content.reserve(size + 1);
    char buffer[256];
    std::size_t remaining = size;
    while (remaining && file.available()) {
        const int n = file.readBytes(buffer, std::min<std::size_t>(remaining, sizeof(buffer)));
        if (n <= 0) break;
        content.concat(buffer, n);
        remaining -= n;
    }
    file.close();
    return content;
}

String read_tail(const String& path, std::size_t max_bytes) {
    flush_for(path);
    const Timed timed("read_tail", path);
    if (!active) return {};
    File file = active->open(path, FILE_READ);
    if (!file || file.isDirectory()) return {};
    const std::size_t size = file.size();
    std::size_t start = size > max_bytes ? size - max_bytes : 0;
    file.seek(start);
    String content;
    content.reserve(std::min(size, max_bytes) + 1);
    char buffer[256];
    while (file.available()) {
        const int n = file.readBytes(buffer, sizeof(buffer));
        if (n <= 0) break;
        content.concat(buffer, n);
    }
    file.close();
    if (start > 0) {  // drop the partial first line in place: no second copy of the tail
        const int newline = content.indexOf('\n');
        if (newline >= 0) content.remove(0, newline + 1);
        else content = String();
    }
    return content;
}

bool overwrite(const String& path, const String& content) {
    const Timed timed("overwrite", path);
    if (!active || !ensure_parent(path)) return false;
    File output = active->open(path, FILE_WRITE);
    if (!output) {
        remember(path, kUnknown);
        return false;
    }
    const bool ok = output.write(reinterpret_cast<const uint8_t*>(content.c_str()), content.length()) ==
                    content.length();
    output.close();
    remember(path, ok ? static_cast<long>(content.length()) : kUnknown);
    return ok;
}

bool write_text(const String& path, const String& content) {
    const Timed timed("write_text", path);
    if (!active || !ensure_parent(path)) return false;
    const String temp = path + ".tmp";
    if (cached_size(temp) != -1) active->remove(temp);
    File output = active->open(temp, FILE_WRITE);
    if (!output) return false;
    const bool ok = output.write(reinterpret_cast<const uint8_t*>(content.c_str()),
                                 content.length()) == content.length();
    output.close();
    if (!ok) {
        active->remove(temp);
        return false;
    }
    if (cached_size(path) != -1) active->remove(path);
    remember(temp, -1);
    if (!active->rename(temp, path)) {
        remember(path, kUnknown);
        return false;
    }
    remember(path, content.length());
    mirror_file(path);
    return true;
}

namespace {
// Appends `data` with one open (the old code opened twice per line: once to
// check the size for rotation, once to append).
bool write_out(const String& path, const String& data, bool rotate) {
    if (!active || !ensure_parent(path)) return false;
    xSemaphoreTake(append_mutex, portMAX_DELAY);
    struct Release {
        ~Release() { xSemaphoreGive(append_mutex); }
    } release;
    long before = cached_size(path);
    File output = active->open(path, FILE_APPEND);
    if (!output) return false;
    if (rotate && output.size() > (active_kind == "tf" ? kRotateBytesTf : kRotateBytesFlash)) {
        output.close();
        const String old = path.substring(0, path.lastIndexOf('.')) + ".1.jsonl";
        active->remove(old);
        active->rename(path, old);
        remember(old, kUnknown);
        before = -1;  // a fresh file
        output = active->open(path, FILE_APPEND);
        if (!output) return false;
    }
    const bool ok = output.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.length()) == data.length();
    output.close();
    if (ok) grew(path, before, data.length());
    else remember(path, kUnknown);
    if (ok) mirror_file(path);  // no-op unless mirrored (notes)
    return ok;
}

// Write-behind for append-only logs (runtime log, archive): each card write
// can stall for seconds on this card after earlier writes, and one line per
// write made thousands of them.  Lines are kept in RAM for up to
// kBufferMs or kBufferBytes and written in one go; anything that reads the
// file flushes it first.
constexpr std::size_t kBufferBytes = 2048;
constexpr unsigned long kBufferMs = 5000;
struct Buffered {
    String data;
    unsigned long since = 0;
    bool rotate = false;
};
std::map<String, Buffered> buffered;
SemaphoreHandle_t buffer_mutex = xSemaphoreCreateMutex();

void flush_where(const std::function<bool(const String&, const Buffered&)>& pick) {
    std::vector<std::pair<String, Buffered>> due;
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    for (auto it = buffered.begin(); it != buffered.end();) {
        if (pick(it->first, it->second)) {
            due.emplace_back(it->first, std::move(it->second));
            it = buffered.erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(buffer_mutex);
    for (const auto& entry : due) write_out(entry.first, entry.second.data, entry.second.rotate);
}

void flush_for(const String& path) {  // the file itself, or everything under a directory
    flush_where([&](const String& p, const Buffered&) { return p == path || p.startsWith(path + "/"); });
}
}  // namespace

bool append_line(const String& path, const String& line) {
    const Timed timed("append_line", path);
    flush_for(path);
    return write_out(path, line + "\n", true);
}

bool append_buffered(const String& path, const String& text, bool rotate) {
    if (!active) return false;
    bool full = false;
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    Buffered& entry = buffered[path];
    if (entry.data.isEmpty()) entry.since = millis();
    entry.data += text;
    entry.rotate = rotate;
    full = entry.data.length() >= kBufferBytes;
    xSemaphoreGive(buffer_mutex);
    if (full) flush_for(path);
    return true;
}

void flush() { flush_where([](const String&, const Buffered&) { return true; }); }

void tick() {
    const unsigned long now = millis();
    flush_where([now](const String&, const Buffered& b) { return now - b.since >= kBufferMs; });
}

bool append(const String& path, const String& text) {
    const Timed timed("append", path);
    if (!active || !ensure_parent(path)) return false;
    xSemaphoreTake(append_mutex, portMAX_DELAY);
    const long before = cached_size(path);
    File output = active->open(path, FILE_APPEND);
    const bool ok = output && output.write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length()) ==
                                  text.length();
    if (output) output.close();
    if (ok) grew(path, before, text.length());
    else remember(path, kUnknown);
    xSemaphoreGive(append_mutex);
    if (ok) mirror_file(path);
    return ok;
}

std::size_t size(const String& path) {
    flush_for(path);
    const long known = cached_size(path);
    if (known != kUnknown) return known > 0 ? known : 0;
    const Timed timed("size", path);
    if (!active) return 0;
    File file = active->open(path, FILE_READ);
    if (!file) {
        remember(path, -1);
        return 0;
    }
    if (file.isDirectory()) {
        file.close();
        return 0;
    }
    const std::size_t bytes = file.size();
    file.close();
    remember(path, bytes);
    return bytes;
}

String read_range(const String& path, std::size_t offset, std::size_t max_bytes) {
    flush_for(path);
    const Timed timed("read_range", path);
    if (!active) return {};
    File file = active->open(path, FILE_READ);
    if (!file || file.isDirectory() || offset >= file.size()) return {};
    file.seek(offset);
    String content;
    content.reserve(std::min<std::size_t>(file.size() - offset, max_bytes) + 1);
    char buffer[256];
    std::size_t remaining = max_bytes;
    while (remaining && file.available()) {
        const int n = file.readBytes(buffer, std::min<std::size_t>(remaining, sizeof(buffer)));
        if (n <= 0) break;
        content.concat(buffer, n);
        remaining -= n;
    }
    file.close();
    return content;
}

bool read_range_bytes(const String& path, std::size_t offset, std::size_t length, std::vector<uint8_t>& out) {
    flush_for(path);
    const Timed timed("read_range_bytes", path);
    out.clear();
    if (!active) return false;
    File file = active->open(path, FILE_READ);
    if (!file || file.isDirectory() || offset + length > file.size() || !file.seek(offset)) return false;
    out.resize(length);
    const std::size_t n = file.read(out.data(), length);
    file.close();
    return n == length;
}

bool read_bytes(const String& path, std::vector<uint8_t>& out, std::size_t max_bytes) {
    flush_for(path);
    const Timed timed("read_bytes", path);
    out.clear();
    if (!active) return false;
    File file = active->open(path, FILE_READ);
    if (!file || file.isDirectory() || file.size() > max_bytes) return false;
    out.resize(file.size());
    const std::size_t n = file.read(out.data(), out.size());
    file.close();
    return n == out.size();
}

bool rename(const String& from, const String& to) {
    const Timed timed("rename", from);
    if (!active || !ensure_parent(to)) return false;
    if (cached_size(to) != -1) active->remove(to);
    const long moved = cached_size(from);
    const bool ok = active->rename(from, to);
    remember(from, ok ? -1 : kUnknown);
    // A source the cache calls missing was written through fs(): size unknown.
    remember(to, ok && moved >= 0 ? moved : kUnknown);
    return ok;
}

bool remove(const String& path) {
    if (cached_size(path) == -1) return false;  // known missing: no directory search on the card
    const Timed timed("remove", path);
    const bool ok = active && active->remove(path);
    remember(path, -1);
    if (ok) mirror_file(path);
    return ok;
}

void changed(const String& path) {
    remember(path, kUnknown);
    mirror_file(path);
}

String check_card() {
    static uint32_t last_check = 0;
    static uint32_t slow_seen = 0;
    static bool lost = false;
    if (active_kind != "tf") return "";
    const bool suspect = slow_count.load() != slow_seen;
    const uint32_t interval = lost ? 30000 : suspect ? 0 : 60000;
    if (last_check && millis() - last_check < interval) return "";
    last_check = millis();
    slow_seen = slow_count.load();
    if (SD.exists("/vat")) return "";
    // The SD driver marks the card "not initialised" after one command
    // timeout and never retries by itself (sd_diskio.cpp): every call fails
    // while it still looks mounted.  Only a fresh mount brings it back.
    Serial.println("[sd] /vat unreadable: remounting");
    const bool remounted = mount_card() && SD.exists("/vat");
    xSemaphoreTake(sizes_mutex, portMAX_DELAY);
    sizes.clear();
    xSemaphoreGive(sizes_mutex);
    known_dirs.clear();
    if (!remounted) {
        lost = true;
        return "lost";
    }
    lost = false;
    return "remounted";
}

uint32_t card_hz() { return sd_hz; }
String bench(uint32_t hz) {
    if (active_kind != "tf") return "{\"error\":\"no card\"}";
    SD.end();
    SPI.end();
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    if (!SD.begin(kSdCs, SPI, hz, "/sd", kSdMaxFiles, false) || SD.cardType() == CARD_NONE) {
        return "{\"error\":\"remount failed\"}";
    }
    known_dirs.clear();
    struct Stat {
        const char* name;
        uint32_t n = 0, total = 0, max = 0, over = 0;
        void add(uint32_t ms) { ++n; total += ms; max = std::max(max, ms); over += ms > 400; }
    } append_s{"append"}, write_s{"write"}, read_s{"read"}, size_s{"size"}, remove_s{"remove"};
    const String dir = "/vat/cache/bench";
    SD.mkdir(dir);
    const String line = String("bench line 0123456789abcdefghijklmnopqrstuvwxyz 0123456789abcdefghijklmnopqrstuvwxyz\n");
    for (int i = 0; i < 100; ++i) {
        const unsigned long t = millis();
        File f = SD.open(dir + "/append.log", FILE_APPEND);
        if (f) { f.print(line); f.close(); }
        append_s.add(millis() - t);
    }
    for (int i = 0; i < 30; ++i) {
        const String path = dir + "/f" + String(i) + ".txt";
        unsigned long t = millis();
        File w = SD.open(path, FILE_WRITE);
        if (w) { for (int k = 0; k < 8; ++k) w.print(line); w.close(); }
        write_s.add(millis() - t);
        t = millis();
        File r = SD.open(path, FILE_READ);
        if (r) { while (r.available()) r.read(); r.close(); }
        read_s.add(millis() - t);
        t = millis();
        File z = SD.open(path, FILE_READ);
        if (z) { (void)z.size(); z.close(); }
        size_s.add(millis() - t);
    }
    for (int i = 0; i < 30; ++i) {
        const unsigned long t = millis();
        SD.remove(dir + "/f" + String(i) + ".txt");
        remove_s.add(millis() - t);
    }
    SD.remove(dir + "/append.log");
    SD.rmdir(dir);
    String json = "{\"hz\":" + String(hz);
    for (Stat* st : {&append_s, &write_s, &read_s, &size_s, &remove_s}) {
        json += ",\"" + String(st->name) + "\":{\"n\":" + String(st->n) + ",\"avg\":" +
                String(st->n ? st->total / st->n : 0) + ",\"max\":" + String(st->max) + ",\"over400\":" +
                String(st->over) + "}";
    }
    return json + "}";
}

String slow_report() { return String(slow_count.load()) + "/" + String(slow_max_ms.load()) + "ms"; }
bool degraded() { return degraded_mode; }

bool remove_tree(const String& path) {
    const Timed timed("remove_tree", path);
    if (!active) return false;
    known_dirs.clear();
    forget_under(path);
    remove_tree_on(*active, path);
    return !active->exists(path);
}

String list_dir(const String& path) {
    flush_for(path);
    const Timed timed("list_dir", path);
    String json = "[";
    if (!active) return "[]";
    File root = active->open(path);
    if (!root || !root.isDirectory()) return "[]";
    File entry = root.openNextFile();
    bool first = true;
    while (entry) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"";
        json += entry.name();
        json += "\",\"size\":";
        json += String(static_cast<unsigned long>(entry.size()));
        json += ",\"dir\":";
        json += entry.isDirectory() ? "true" : "false";
        json += "}";
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    json += "]";
    return json;
}

}  // namespace vat::storage
