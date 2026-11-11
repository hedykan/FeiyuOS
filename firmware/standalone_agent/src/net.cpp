#include "net.h"

#include <atomic>

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <sdkconfig.h>

#include "arena.h"
#include "common.h"
#include "storage.h"

namespace vat::net {
namespace detail {
void* tls_calloc(size_t n, size_t size);
void tls_free(void* ptr);
}  // namespace detail
namespace {

WifiState wifi_state = WifiState::Off;

// mbedTLS needs two ~16.7 KB contiguous buffers per connection.  Long-running
// use fragments the heap until no such block is left even with 90 KB free, so
// two blocks are claimed while the heap is still clean and handed back only
// for the duration of a TLS connection.
// One block per mbedTLS record buffer, each sized from the build's
// configuration: the receive buffer (16 KB + headroom) and the send buffer,
// which the custom libraries cut to 4 KB.  The reserve used to keep 17.5 KB for
// both: 12.5 KB held for nothing with the custom libraries.
constexpr std::size_t kTlsReserveBytes[2] = {CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN + 1536,
                                             CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN + 1024};
//
// mbedTLS takes its record buffers straight from these blocks (tls_calloc,
// installed with mbedtls_platform_set_calloc_free on the first claim), so they
// stay where they were claimed.  Handing them back to
// the heap for each connection let other allocations take part of the hole,
// and the reserve came back somewhere else every time: after an E2E run the
// largest free block was 32 KB of 86 KB free.
struct Slot {
    void* block;
    std::size_t bytes;
    bool tls;   // mbedTLS is using it
    bool lent;  // the boot screen is using it
};
Slot slots[2] = {{nullptr, kTlsReserveBytes[0], false, false}, {nullptr, kTlsReserveBytes[1], false, false}};
portMUX_TYPE slots_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t slot_hits = 0;    // mbedTLS allocations served from the reserve
uint32_t slot_misses = 0;  // large ones that found no free block (heap instead)

void reserve_blocks() {
    static bool installed = false;
    if (!installed) {  // before the first TLS connection; Wi-Fi's earlier ones free through it to the heap
        mbedtls_platform_set_calloc_free(detail::tls_calloc, detail::tls_free);
        installed = true;
    }
    std::size_t held = 0;
    for (auto& s : slots) {
        if (!s.block) {
            void* block = heap_caps_malloc(s.bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            portENTER_CRITICAL(&slots_lock);
            s.block = block;
            portEXIT_CRITICAL(&slots_lock);
        }
        if (s.block) held += s.bytes;
    }
    arena::note("tls_reserve", held);
}

// Frees the blocks nobody is using (an SSH session, a recording need them).
void release_blocks() {
    std::size_t held = 0;
    for (auto& s : slots) {
        void* gone = nullptr;
        portENTER_CRITICAL(&slots_lock);
        if (s.block && !s.tls && !s.lent) {
            gone = s.block;
            s.block = nullptr;
        }
        portEXIT_CRITICAL(&slots_lock);
        free(gone);
        if (s.block) held += s.bytes;
    }
    arena::give_back("tls_reserve");
    if (held) arena::note("tls_reserve", held);
}
String setup_ap_name;
constexpr uint32_t kSampleRate = 16000;

bool split_url(const String& url, String& host, uint16_t& port, bool& secure) {
    secure = url.startsWith("https://");
    if (!secure && !url.startsWith("http://")) return false;
    const int start = secure ? 8 : 7;
    int end = url.length();
    for (const char stop : {'/', '?', '#'}) {
        const int at = url.indexOf(stop, start);
        if (at >= 0 && at < end) end = at;
    }
    host = url.substring(start, end);
    if (host.indexOf('@') >= 0) return false;  // no user:password@ in the URL
    port = secure ? 443 : 80;
    const int colon = host.indexOf(':');
    if (colon >= 0) {
        port = host.substring(colon + 1).toInt();
        host = host.substring(0, colon);
    }
    return !host.isEmpty();
}

class Base64PcmStream : public Stream {
public:
    Base64PcmStream(const String& prefix, File& pcm, const String& suffix)
        : prefix_(prefix), suffix_(suffix), pcm_(pcm) {
        const uint32_t pcm_size = pcm_.size();
        raw_total_ = 44 + pcm_size;
        b64_total_ = ((raw_total_ + 2) / 3) * 4;
        total_ = prefix_.length() + b64_total_ + suffix_.length();
        build_header(pcm_size);
    }

    size_t total() const { return total_; }
    int available() override { return static_cast<int>(total_ - position_); }
    int read() override {
        uint8_t value;
        return readBytes(reinterpret_cast<char*>(&value), 1) == 1 ? value : -1;
    }
    int peek() override { return -1; }
    size_t write(uint8_t) override { return 0; }

    size_t readBytes(char* buffer, size_t length) override {
        size_t produced = 0;
        while (produced < length && position_ < total_) {
            if (position_ < prefix_.length()) {
                const size_t n = std::min(length - produced, prefix_.length() - position_);
                memcpy(buffer + produced, prefix_.c_str() + position_, n);
                produced += n;
                position_ += n;
                continue;
            }
            const size_t b64_end = prefix_.length() + b64_total_;
            if (position_ < b64_end) {
                if (out_pos_ >= out_len_) refill();
                if (out_len_ == 0) break;
                const size_t n = std::min(length - produced, out_len_ - out_pos_);
                memcpy(buffer + produced, out_ + out_pos_, n);
                out_pos_ += n;
                produced += n;
                position_ += n;
                continue;
            }
            const size_t offset = position_ - b64_end;
            const size_t n = std::min(length - produced, suffix_.length() - offset);
            memcpy(buffer + produced, suffix_.c_str() + offset, n);
            produced += n;
            position_ += n;
        }
        return produced;
    }

private:
    void build_header(uint32_t pcm_size) {
        auto put32 = [this](int at, uint32_t v) {
            header_[at] = v; header_[at + 1] = v >> 8; header_[at + 2] = v >> 16; header_[at + 3] = v >> 24;
        };
        auto put16 = [this](int at, uint16_t v) { header_[at] = v; header_[at + 1] = v >> 8; };
        memcpy(header_, "RIFF", 4); put32(4, pcm_size + 36); memcpy(header_ + 8, "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, kSampleRate); put32(28, kSampleRate * 2);
        put16(32, 2); put16(34, 16); memcpy(header_ + 36, "data", 4); put32(40, pcm_size);
    }

    size_t read_raw(uint8_t* output, size_t length) {
        size_t produced = 0;
        while (produced < length && raw_pos_ < raw_total_) {
            if (raw_pos_ < 44) {
                const size_t n = std::min(length - produced, static_cast<size_t>(44 - raw_pos_));
                memcpy(output + produced, header_ + raw_pos_, n);
                produced += n;
                raw_pos_ += n;
            } else {
                const int n = pcm_.read(output + produced, length - produced);
                if (n <= 0) break;
                produced += n;
                raw_pos_ += n;
            }
        }
        return produced;
    }

    void refill() {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint8_t raw[768];
        const size_t n = read_raw(raw, sizeof(raw));
        out_len_ = 0;
        out_pos_ = 0;
        for (size_t i = 0; i < n; i += 3) {
            const uint32_t a = raw[i];
            const uint32_t b = i + 1 < n ? raw[i + 1] : 0;
            const uint32_t c = i + 2 < n ? raw[i + 2] : 0;
            const uint32_t triple = (a << 16) | (b << 8) | c;
            out_[out_len_++] = table[(triple >> 18) & 63];
            out_[out_len_++] = table[(triple >> 12) & 63];
            out_[out_len_++] = i + 1 < n ? table[(triple >> 6) & 63] : '=';
            out_[out_len_++] = i + 2 < n ? table[triple & 63] : '=';
        }
    }

    String prefix_;
    String suffix_;
    File& pcm_;
    uint8_t header_[44];
    size_t raw_total_ = 0;
    size_t raw_pos_ = 0;
    size_t b64_total_ = 0;
    size_t total_ = 0;
    size_t position_ = 0;
    char out_[1024];
    size_t out_len_ = 0;
    size_t out_pos_ = 0;
};

// Keeps at most `limit` bytes of a response body; refuses the rest.
class LimitedSink : public Stream {
public:
    LimitedSink(String& out, std::size_t limit) : out_(out), limit_(limit) {}
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t n) override {
        if (out_.length() + n > limit_) {
            over = true;
            return 0;
        }
        out_.concat(reinterpret_cast<const char*>(data), n);
        return n;
    }
    bool over = false;

private:
    String& out_;
    std::size_t limit_;
};

using Headers = std::vector<std::pair<String, String>>;

class SegmentStream : public Stream {
public:
    explicit SegmentStream(const std::vector<Segment>& parts) : parts_(parts) {
        for (const auto& part : parts_) total_ += part.length;
    }
    size_t total() const { return total_; }
    int available() override { return static_cast<int>(total_ - position_); }
    int read() override {
        char c;
        return readBytes(&c, 1) == 1 ? static_cast<uint8_t>(c) : -1;
    }
    int peek() override { return -1; }
    size_t write(uint8_t) override { return 0; }
    size_t readBytes(char* buffer, size_t length) override {
        size_t produced = 0;
        while (produced < length && index_ < parts_.size()) {
            const auto& part = parts_[index_];
            const size_t n = std::min(length - produced, part.length - offset_);
            if (part.path) {
                if (!file_) file_ = storage::fs().open(part.path, FILE_READ);
                if (file_ && offset_ == 0) file_.seek(part.offset);
                const int got = file_ ? file_.read(reinterpret_cast<uint8_t*>(buffer + produced), n) : 0;
                const size_t filled = got > 0 ? static_cast<size_t>(got) : 0;
                if (filled < n) memset(buffer + produced + filled, ' ', n - filled);
            } else {
                memcpy(buffer + produced, part.data + offset_, n);
            }
            produced += n;
            offset_ += n;
            position_ += n;
            if (offset_ == part.length) {
                if (file_) file_.close();
                ++index_;
                offset_ = 0;
            }
        }
        return produced;
    }

    ~SegmentStream() {
        if (file_) file_.close();
    }

private:
    const std::vector<Segment>& parts_;
    File file_;  // the file segment being sent (one open file at a time)
    size_t total_ = 0;
    size_t position_ = 0;
    size_t index_ = 0;
    size_t offset_ = 0;
};

// One HTTPS exchange.  The TLS socket is opened here so its fd can be
// registered for cancellation before any blocking read happens.
HttpResult exchange(const char* method, const String& url, const String& api_key,
                    const String* body, Stream* stream, size_t stream_size,
                    uint32_t timeout_ms, const String* save_path = nullptr,
                    const Headers* headers = nullptr, std::size_t max_body = 0) {
    HttpResult result;
    String host;
    uint16_t port;
    bool secure = true;
    if (!split_url(url, host, port, secure)) {
        result.status = -1;
        result.error = "invalid url";
        return result;
    }
    if (cancel::requested()) {
        result.status = -2;
        result.error = "cancelled";
        return result;
    }
    if (secure) reserve_blocks();  // mbedTLS's record buffers come from the reserve (tls_calloc)
    NetworkClientSecure tls;
    NetworkClient plain;
    NetworkClient& client = secure ? static_cast<NetworkClient&>(tls) : plain;
    bool connected;
    if (secure) {
        tls.setInsecure();
        tls.setTimeout(timeout_ms / 1000 + 1);
        tls.setHandshakeTimeout(12);
        connected = tls.connect(host.c_str(), port, 12000);
    } else {
        plain.setTimeout(timeout_ms / 1000 + 1);
        connected = plain.connect(host.c_str(), port, 12000);
    }
    if (!connected) {
        char reason[96] = {};
        const int code = secure ? tls.lastError(reason, sizeof(reason)) : 0;
        result.status = -3;
        result.error = String(secure ? "tls" : "tcp") + " connect failed " + String(code) + " " + reason +
                       " heap=" + ESP.getFreeHeap() + " block=" + ESP.getMaxAllocHeap() + " wifi=" + WiFi.status();
        runtime_event("tls_fail", host + " " + result.error);
        return result;
    }
    cancel::set_socket(secure ? tls.fd() : plain.fd());
    HTTPClient http;
    http.setConnectTimeout(12000);
    // HTTPClient keeps this in 16 bits: 90000 became 24464 and a round that wrote a
    // program timed out sooner than before.
    http.setTimeout(static_cast<uint16_t>(std::min<uint32_t>(timeout_ms, 65000)));
    http.setReuse(false);
    if (!http.begin(client, url)) {
        cancel::clear_socket();
        result.status = -1;
        result.error = "http begin failed";
        return result;
    }
    if (!api_key.isEmpty()) http.addHeader("Authorization", "Bearer " + api_key);
    if (headers) {
        for (const auto& header : *headers) http.addHeader(header.first, header.second);
    } else {
        http.addHeader("Content-Type", "application/json");
    }
    http.addHeader("User-Agent", "voice-agent-terminal/1.0");
    if (stream) {
        result.status = http.sendRequest(method, stream, stream_size);
    } else if (body) {
        result.status = http.sendRequest(method, reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())), body->length());
    } else {
        result.status = http.sendRequest(method);
    }
    // Stream::timedRead() busy-waits without yielding; reading the body from a
    // socket shut down by Esc would spin until the timeout and trip the task
    // watchdog.  From here on, cancel only sets the flag; the body is short.
    cancel::clear_socket();
    if (result.status > 0 && !cancel::requested() && save_path && result.ok()) {
        // Large bodies go straight to storage in small blocks.
        storage::remove(*save_path);
        File file = storage::ensure_parent(*save_path) ? storage::fs().open(*save_path, FILE_WRITE) : File();
        if (!file || http.writeToStream(&file) < 0) result.error = "response not saved";
        if (file) file.close();
        storage::changed(*save_path);  // written through fs(): drop the cached size
    } else if (result.status > 0 && !cancel::requested() && max_body > 0) {
        LimitedSink sink(result.body, max_body);
        http.writeToStream(&sink);
        if (sink.over) result.error = "response too large";
    } else if (result.status > 0 && !cancel::requested()) {
        result.body = http.getString();
    } else {
        result.error = HTTPClient::errorToString(result.status);
    }
    http.end();
    cancel::clear_socket();
    client.stop();
    if (cancel::requested()) {
        result.status = -2;
        result.error = "cancelled";
    }
    return result;
}

bool retryable(const HttpResult& result) {
    if (result.status == -2) return false;  // cancelled
    if (result.status <= 0) return true;
    if (result.status >= 500) return true;
    return result.ok() && result.body.isEmpty();
}

}  // namespace

void* reserve_block(int index, std::size_t& bytes) {
    if (index < 0 || index > 1) return nullptr;
    reserve_blocks();
    Slot& s = slots[index];
    portENTER_CRITICAL(&slots_lock);
    s.lent = s.block != nullptr;
    portEXIT_CRITICAL(&slots_lock);
    bytes = s.block ? s.bytes : 0;
    return s.block;
}

void end_lending() {
    portENTER_CRITICAL(&slots_lock);
    for (auto& s : slots) s.lent = false;
    portEXIT_CRITICAL(&slots_lock);
}

void reserve_stats(uint32_t& hits, uint32_t& misses) {
    hits = slot_hits;
    misses = slot_misses;
}

namespace detail {

// The smallest free reserve block that holds `bytes`, now marked in use.
void* take_slot(std::size_t bytes) {
    void* got = nullptr;
    portENTER_CRITICAL(&slots_lock);
    Slot* best = nullptr;
    for (auto& s : slots) {
        if (s.block && !s.tls && !s.lent && bytes <= s.bytes && (!best || s.bytes < best->bytes)) best = &s;
    }
    if (best) {
        best->tls = true;
        got = best->block;
        ++slot_hits;
    } else {
        ++slot_misses;
    }
    portEXIT_CRITICAL(&slots_lock);
    return got;
}

bool give_slot(void* block) {
    bool mine = false;
    portENTER_CRITICAL(&slots_lock);
    for (auto& s : slots) {
        if (s.block == block && s.tls) {
            s.tls = false;
            mine = true;
        }
    }
    portEXIT_CRITICAL(&slots_lock);
    return mine;
}

}  // namespace detail

void release_reserve() { release_blocks(); }
void restore_reserve() { reserve_blocks(); }

bool connect_station(const String& ssid, const String& password, uint32_t timeout_ms) {
    if (ssid.isEmpty()) return false;
    wifi_state = WifiState::Connecting;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("vat-cardputer");
    // Still on another network: leave it first.  Otherwise status() stays
    // WL_CONNECTED and the old connection passes for the new one.
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != ssid) {
        WiFi.disconnect(false, false);
        const unsigned long until = millis() + 3000;
        while (WiFi.status() == WL_CONNECTED && millis() < until) delay(50);
    }
    WiFi.setAutoReconnect(true);  // once joined, a dropped link comes back by itself
    WiFi.begin(ssid.c_str(), password.c_str());
    const unsigned long deadline = millis() + timeout_ms;
    const auto on_it = [&ssid] { return WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid; };
    while (!on_it() && millis() < deadline) delay(100);
    if (on_it()) {
        WiFi.setSleep(false);
        reserve_blocks();
        wifi_state = WifiState::Connected;
        return true;
    }
    // Given up: stop the driver retrying in the background.  While it
    // retries, every scan fails at once (connecting comes before scanning).
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    wifi_state = WifiState::Off;
    return false;
}

void start_setup_ap() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[16];
    snprintf(name, sizeof(name), "VAT-%02X%02X", mac[4], mac[5]);
    setup_ap_name = name;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(setup_ap_name.c_str());
    wifi_state = WifiState::SetupAp;
}

WifiState state() {
    if (wifi_state == WifiState::Connected && WiFi.status() != WL_CONNECTED) {
        return WifiState::Connecting;
    }
    return wifi_state;
}

String ip() {
    if (wifi_state == WifiState::SetupAp) return WiFi.softAPIP().toString();
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
}

String ssid() { return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(); }
int rssi() { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

String bssid() { return WiFi.status() == WL_CONNECTED ? WiFi.BSSIDstr() : String(); }

int channel() { return WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0; }

String link_text() {
    if (WiFi.status() != WL_CONNECTED) return {};
    return "bssid=" + WiFi.BSSIDstr() + " ch=" + String(WiFi.channel()) + " rssi=" + String(WiFi.RSSI());
}

String same_ssid_nodes() {
    if (WiFi.status() != WL_CONNECTED) return {};
    const String ssid = WiFi.SSID();
    const String mine = WiFi.BSSIDstr();
    const int found = WiFi.scanNetworks(false, false);
    String out;
    int listed = 0;
    for (int i = 0; i < found && listed < 8; ++i) {
        if (WiFi.SSID(i) != ssid) continue;
        const String b = WiFi.BSSIDstr(i);
        out += String(out.isEmpty() ? "" : " ") + b + " ch=" + String(WiFi.channel(i)) + " rssi=" + String(WiFi.RSSI(i)) +
               (b == mine ? "*" : "");
        ++listed;
    }
    WiFi.scanDelete();
    return out.isEmpty() ? String("(scan found none)") : out;
}

namespace {
std::atomic<uint8_t> link_events{0};    // bit 0: got an address, bit 1: lost the link
std::atomic<uint8_t> lost_reason{0};
bool handler_added = false;
}  // namespace

void report_link_changes() {
    if (!handler_added) {
        handler_added = true;
        WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
            if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) link_events |= 1;
            if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
                lost_reason = info.wifi_sta_disconnected.reason;
                link_events |= 2;
            }
        });
    }
    const uint8_t events = link_events.exchange(0);
    if (events & 2) runtime_event("wifi_lost", "reason=" + String(lost_reason.load()));
    if (events & 1) runtime_event("wifi_link", ip() + " " + link_text());
}
String ap_name() { return setup_ap_name; }

HttpResult post_json_to_file(const String& url, const String& api_key, const String& body,
                             const String& save_path, uint32_t timeout_ms, int attempts) {
    HttpResult result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        result = exchange("POST", url, api_key, &body, nullptr, 0, timeout_ms, &save_path);
        if (result.ok() && !result.error.isEmpty()) result.status = -4;  // body lost on the way to storage
        runtime_event("http_post", url.substring(8, 48) + " status=" + result.status + " attempt=" + attempt +
                                       " saved=" + String(storage::size(save_path)));
        const bool retry = result.status != -2 && (result.status <= 0 || result.status == 429 || result.status >= 500);
        if (!retry || attempt == attempts) break;
        delay(600 * attempt);
    }
    return result;
}

HttpResult post_segments(const String& url, const String& api_key, const std::vector<Segment>& parts,
                         uint32_t timeout_ms, int attempts) {
    HttpResult result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        SegmentStream stream(parts);
        result = exchange("POST", url, api_key, nullptr, &stream, stream.total(), timeout_ms);
        runtime_event("http_post", url.substring(8, 40) + " status=" + result.status + " attempt=" + attempt +
                                       " sent=" + stream.total() + " bytes=" + result.body.length());
        if (!retryable(result) || attempt == attempts) break;
        delay(400 * attempt);
    }
    return result;
}

HttpResult get(const String& url, const String& api_key, uint32_t timeout_ms) {
    return exchange("GET", url, api_key, nullptr, nullptr, 0, timeout_ms);
}

HttpResult request(const Request& r) {
    Headers headers = r.headers;
    const bool has_body = !r.body.isEmpty() || r.method == "POST" || r.method == "PUT" || r.method == "PATCH";
    bool typed = false;
    for (const auto& header : headers) typed = typed || header.first.equalsIgnoreCase("Content-Type");
    if (has_body && !typed) {
        String start = r.body;
        start.trim();
        headers.emplace_back("Content-Type", start.startsWith("{") || start.startsWith("[")
                                                 ? "application/json"
                                                 : "text/plain; charset=utf-8");
    }
    HttpResult result = exchange(r.method.c_str(), r.url, String(), has_body ? &r.body : nullptr, nullptr, 0,
                                 r.timeout_ms, r.save_path.isEmpty() ? nullptr : &r.save_path, &headers, r.max_body);
    if (result.ok() && !r.save_path.isEmpty() && !result.error.isEmpty()) result.status = -4;  // not saved
    String host;
    uint16_t port;
    bool secure;
    split_url(r.url, host, port, secure);
    runtime_event("http_request", r.method + " " + host + " status=" + result.status + " bytes=" +
                                      (r.save_path.isEmpty() ? result.body.length() : storage::size(r.save_path)));
    return result;
}

HttpResult post_pcm_base64(const String& url, const String& api_key,
                           const String& prefix, const String& pcm_path,
                           const String& suffix, uint32_t timeout_ms, int attempts) {
    HttpResult result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        File pcm = storage::fs().open(pcm_path, FILE_READ);
        if (!pcm) {
            result.status = -1;
            result.error = "pcm missing";
            return result;
        }
        Base64PcmStream stream(prefix, pcm, suffix);
        result = exchange("POST", url, api_key, nullptr, &stream, stream.total(), timeout_ms);
        pcm.close();
        runtime_event("http_stream", url.substring(8, 40) + " status=" + result.status +
                                         " attempt=" + attempt + " sent=" + stream.total());
        if (!retryable(result) || attempt == attempts) break;
        delay(400 * attempt);
    }
    return result;
}

}  // namespace vat::net

// mbedTLS allocates through these (all of it: HTTPS, libssh's crypto, Wi-Fi's
// WPA code on its own task).  Record buffers (4 KB and up) come from the
// reserve, the rest from the internal heap.
namespace vat::net::detail {
namespace {
constexpr std::size_t kSlotMinBytes = 4096;
}

void* tls_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return nullptr;
    const std::size_t bytes = n * size;
    if (bytes >= kSlotMinBytes) {
        if (void* block = take_slot(bytes)) {
            memset(block, 0, bytes);
            return block;
        }
    }
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void tls_free(void* ptr) {
    if (ptr && !give_slot(ptr)) heap_caps_free(ptr);
}

}  // namespace vat::net::detail
