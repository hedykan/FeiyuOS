#include "ops_ble.h"

#include <NimBLEDevice.h>
#include <NimBLEBeacon.h>
#include <NimBLEHIDDevice.h>
#include <cJSON.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <vector>

#include "arena.h"
#include "common.h"
#include "config.h"
#include "events.h"
#include "storage.h"
#include "ui_state.h"

namespace vat::ble {
namespace {

constexpr char kName[] = "FeiyuOS";
constexpr int kMaxDevices = 40;          // the table; the device heard longest ago makes room
constexpr uint32_t kRecentMs = 10000;    // "nearby" = heard in the last 10 s
constexpr uint32_t kRssiFreshMs = 5000;  // an RSSI older than this is not current
constexpr int kMaxSubscriptions = 4;
constexpr size_t kNotifyBytes = 20;
constexpr size_t kMaxLogQueue = 48;      // lines between two writes to the card
constexpr int kMaxClients = 2;           // central connections (the keyboard host is a third)
constexpr uint32_t kConnectTimeoutMs = 6000;
constexpr uint16_t kKeyboardAppearance = 0x03C1;
constexpr int kListMax = 25;

// Standard boot-compatible keyboard, report ID 1: modifiers, reserved, six keys; LED output.
const uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,                          // keyboard, report 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,  // modifiers
    0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,              // reserved byte
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,  // LEDs
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                                      // LED padding
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,  // six keys
    0x29, 0x65, 0x81, 0x00, 0xC0};

struct Device {
    bool used = false;
    uint8_t mac[6] = {};
    uint8_t type = 0;
    char name[24] = {};
    int8_t rssi = 0;
    int8_t tx_power = 0;
    bool has_tx = false;
    bool has_company = false;
    uint16_t company = 0;
    uint16_t appearance = 0;
    char service[40] = {};
    bool connectable = false;
    uint32_t first_ms = 0;
    uint32_t last_ms = 0;
    uint32_t count = 0;
};

struct Subscription {
    bool used = false;
    char key[120] = {};  // address/service/characteristic, lowercase
    uint8_t bytes[kNotifyBytes] = {};
    size_t length = 0;
    uint32_t at_ms = 0;
};

enum class Adv : uint8_t { None, Beacon, Pair, Reconnect };

std::mutex lock;  // devices, subscriptions, log queue, computers: host task, worker and UI task
Device* devices = nullptr;  // kMaxDevices, allocated only when BLE starts
Subscription* subscriptions = nullptr;
std::vector<String> log_queue;
uint32_t log_dropped = 0;
String log_path;
std::vector<NimBLEAddress> computers;  // bonded keyboard hosts (identity addresses)
std::vector<uint16_t> subscribed;       // connections that turned on key reports

bool started = false;
String off_reason = "未开启";
volatile uint32_t scan_until = 0;
volatile uint32_t log_until = 0;
volatile int stream_readers = 0;
volatile bool connecting = false;
volatile Adv adv = Adv::None;
volatile uint32_t adv_until = 0;
volatile bool pair_accepted = false;
volatile int hid_peers = 0;
uint32_t heap_taken = 0;
// What runs on after the reply (pairing window, card log, beacon): its end is
// reported on the screen and as a ble://event/... for triggers.
bool logging = false;
uint32_t logged_lines = 0;
int shown_hosts = 0;

NimBLEServer* server = nullptr;
NimBLEHIDDevice* hid = nullptr;
NimBLECharacteristic* keyboard_input = nullptr;

bool before(uint32_t until) { return until != 0 && static_cast<int32_t>(until - millis()) > 0; }

bool parse_mac(const char* text, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) {
        char* end = nullptr;
        const long byte = strtol(text, &end, 16);
        if (end == text || byte < 0 || byte > 255 || end - text > 2) return false;
        out[i] = static_cast<uint8_t>(byte);
        if (i < 5) {
            if (*end != ':' && *end != '-') return false;
            text = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    return true;
}

String mac_text(const uint8_t mac[6]) {
    char text[18];
    snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

// Copies at most `size - 1` bytes without cutting a UTF-8 character.
void copy_utf8(char* out, size_t size, const std::string& in) {
    size_t n = std::min(in.size(), size - 1);
    while (n > 0 && n < in.size() && (static_cast<uint8_t>(in[n]) & 0xC0) == 0x80) --n;
    memcpy(out, in.data(), n);
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<uint8_t>(out[i]) < 0x20 || out[i] == '"' || out[i] == '\\') out[i] = '?';
    }
}

const char* company_name(uint16_t id) {
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x038F: return "小米";
        case 0x027D: return "华为";
        case 0x0157: return "华米";
        case 0x0087: return "Garmin";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        case 0x0171: return "Amazon";
        case 0x0310: return "SGL Italia";
        default: return nullptr;
    }
}

const char* service_name(const String& uuid) {
    String u = uuid;
    u.toLowerCase();
    if (u.startsWith("0x")) u = u.substring(2);
    if (u == "180f") return "电量";
    if (u == "180a") return "设备信息";
    if (u == "1800") return "通用访问";
    if (u == "1801") return "通用属性";
    if (u == "180d") return "心率";
    if (u == "181a") return "环境传感";
    if (u == "1812") return "键盘鼠标（HID）";
    if (u == "1809") return "体温";
    if (u == "181d") return "体重";
    if (u == "fe95") return "小米";
    if (u == "fd6f") return "接触通知";
    if (u.startsWith("6e400001")) return "Nordic 串口";
    return nullptr;
}

String off_text() {
    return off_reason == "未开启" ? String("蓝牙没有开启：在设置 → 蓝牙里打开后重启。")
                                 : "蓝牙不可用：" + off_reason + "。";
}

// --------------------------------------------------------------- observer

void remember(const NimBLEAdvertisedDevice* d) {
    const uint32_t now = millis();
    uint8_t mac[6];
    const std::string address = d->getAddress().toString();
    if (!parse_mac(address.c_str(), mac)) return;
    std::lock_guard<std::mutex> guard(lock);
    if (!devices) return;
    Device* slot = nullptr;
    Device* oldest = nullptr;
    for (int i = 0; i < kMaxDevices; ++i) {
        Device& x = devices[i];
        if (x.used && memcmp(x.mac, mac, 6) == 0) {
            slot = &x;
            break;
        }
        if (!x.used) {
            if (!oldest || oldest->used) oldest = &x;
        } else if (!oldest || (oldest->used && x.last_ms - oldest->last_ms > 0x80000000u)) {
            oldest = &x;
        }
    }
    if (!slot) {
        slot = oldest;
        *slot = Device();
        slot->used = true;
        memcpy(slot->mac, mac, 6);
        slot->first_ms = now;
    }
    slot->type = d->getAddress().getType();
    slot->rssi = d->getRSSI();
    slot->last_ms = now;
    ++slot->count;
    slot->connectable = d->isConnectable();
    if (d->haveName()) copy_utf8(slot->name, sizeof slot->name, d->getName());
    if (d->haveManufacturerData()) {
        const std::string data = d->getManufacturerData();
        if (data.size() >= 2) {
            slot->has_company = true;
            slot->company = static_cast<uint8_t>(data[0]) | (static_cast<uint8_t>(data[1]) << 8);
        }
    }
    if (d->haveTXPower()) {
        slot->has_tx = true;
        slot->tx_power = d->getTXPower();
    }
    if (d->haveAppearance()) slot->appearance = d->getAppearance();
    if (d->getServiceUUIDCount() > 0) copy_utf8(slot->service, sizeof slot->service, d->getServiceUUID(0).toString());

    if (!before(log_until)) return;
    if (log_queue.size() >= kMaxLogQueue) {
        ++log_dropped;
        return;
    }
    String line = "{\"ms\":" + String(now) + ",\"addr\":\"" + mac_text(mac) + "\",\"rssi\":" + String(slot->rssi);
    if (slot->name[0]) line += String(",\"name\":\"") + slot->name + "\"";
    if (slot->has_company) line += ",\"company\":" + String(slot->company);
    if (slot->service[0]) line += String(",\"service\":\"") + slot->service + "\"";
    if (slot->has_tx) line += ",\"tx\":" + String(slot->tx_power);
    line += String(",\"connectable\":") + (slot->connectable ? "true" : "false") + "}";
    log_queue.push_back(line);
}

class Listener : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* d) override { remember(d); }
};
Listener listener;

void update_scan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    const bool want = !connecting && (before(scan_until) || before(log_until) || stream_readers > 0);
    if (want && !scan->isScanning()) {
        scan->start(0, false, true);  // until stopped
    } else if (!want && scan->isScanning()) {
        scan->stop();
    }
}

void flush_log(bool force = false) {
    static uint32_t last = 0;
    if (!force && millis() - last < 1000) return;
    last = millis();
    std::vector<String> lines;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        lines.swap(log_queue);
        dropped = log_dropped;
        log_dropped = 0;
    }
    if (lines.empty() && dropped == 0) return;
    String text;
    for (const auto& l : lines) text += l + "\n";
    if (dropped) text += "{\"dropped\":" + String(dropped) + "}\n";
    if (!log_path.isEmpty()) storage::append(log_path, text);
    logged_lines += lines.size();
}

String format_device(const Device& d, int index, uint32_t now) {
    String line = String(index) + ". " + (d.name[0] ? String(d.name) : String("（无名）")) + " | " + mac_text(d.mac) +
                  " | " + String(d.rssi) + " dBm";
    if (d.has_company) {
        const char* c = company_name(d.company);
        line += " | " + (c ? String(c) : "厂商 0x" + String(d.company, HEX));
    }
    if (d.service[0]) {
        const char* s = service_name(d.service);
        line += " | 服务 " + (s ? String(s) : String(d.service));
    }
    if (d.connectable) line += " | 可连接";
    line += " | " + String(d.count) + " 次";
    const uint32_t ago = (now - d.last_ms) / 1000;
    if (ago >= 2) line += " | " + String(ago) + " 秒前";
    return line;
}

// The devices heard since `since_ms` (0: all), strongest first.
String list_text(uint32_t since_ms) {
    std::vector<Device> heard;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (int i = 0; i < kMaxDevices; ++i) {
            const Device& d = devices[i];
            if (d.used && (since_ms == 0 || static_cast<int32_t>(d.last_ms - since_ms) >= 0)) heard.push_back(d);
        }
    }
    if (heard.empty()) return "附近没有听到蓝牙设备（只能听到 BLE 广播；经典蓝牙耳机、音箱配对后通常不广播）";
    std::sort(heard.begin(), heard.end(), [](const Device& a, const Device& b) { return a.rssi > b.rssi; });
    const uint32_t now = millis();
    String out = "听到 " + String(heard.size()) + " 个设备（按信号强弱；-50 很近，-90 很远）：";
    for (size_t i = 0; i < heard.size() && i < kListMax; ++i) out += "\n" + format_device(heard[i], i + 1, now);
    if (heard.size() > kListMax) out += "\n…还有 " + String(heard.size() - kListMax) + " 个较弱的";
    return out;
}

const Device* find_device(const uint8_t mac[6]) {  // caller holds the lock
    for (int i = 0; devices && i < kMaxDevices; ++i)
        if (devices[i].used && memcmp(devices[i].mac, mac, 6) == 0) return &devices[i];
    return nullptr;
}

// ------------------------------------------------------- advertising (shared)

void start_keyboard_advertising(bool pairing) {
    NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
    a->stop();
    a->reset();
    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setAppearance(kKeyboardAppearance);
    data.addServiceUUID(hid->getHidService()->getUUID());
    data.setName(kName);
    a->setAdvertisementData(data);
    a->setConnectableMode(BLE_GAP_CONN_MODE_UND);
    a->setMinInterval(pairing ? 48 : 800);  // 30 ms while pairing, 0.5 s while waiting for a known computer
    a->setMaxInterval(pairing ? 96 : 1600);
    a->start();
}

bool known_computer(const NimBLEAddress& id) {  // caller holds the lock
    return std::find(computers.begin(), computers.end(), id) != computers.end();
}

void forget_subscription(uint16_t handle) {  // caller holds the lock
    subscribed.erase(std::remove(subscribed.begin(), subscribed.end(), handle), subscribed.end());
}

// Encrypted links to paired computers: the only ones keystrokes go to.
int count_hosts() {
    int n = 0;
    std::lock_guard<std::mutex> guard(lock);
    for (uint8_t i = 0; server && i < server->getConnectedCount(); ++i) {
        const NimBLEConnInfo info = server->getPeerInfo(i);
        if (info.isEncrypted() && info.isBonded() && known_computer(info.getIdAddress())) ++n;
    }
    return n;
}

class Host : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
        {
            std::lock_guard<std::mutex> guard(lock);
            forget_subscription(info.getConnHandle());
        }
        hid_peers = count_hosts();
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        if (!info.isEncrypted()) {
            server->disconnect(info);
            return;
        }
        const NimBLEAddress id = info.getIdAddress();
        bool accept;
        {
            std::lock_guard<std::mutex> guard(lock);
            accept = known_computer(id);
            if (adv == Adv::Pair && before(adv_until)) {  // new or paired again: the window has done its job
                if (!accept) computers.push_back(id);
                accept = true;
                pair_accepted = true;
            }
        }
        if (!accept) {  // paired outside the window: forget it again
            NimBLEDevice::deleteBond(id);
            server->disconnect(info);
            return;
        }
        hid_peers = count_hosts();
    }
};
Host host;

void update_advertising() {
    NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
    if (pair_accepted) {
        pair_accepted = false;
        adv = Adv::None;
        adv_until = 0;
        shown_hosts = 1;  // "paired" says it: no separate "connected"
        runtime_event("ble", "keyboard paired");
        ui::set_notice("蓝牙键盘：电脑已配对，可以打字了");
        events::emit("ble://event/paired");
    }
    if ((adv == Adv::Beacon || adv == Adv::Pair) && adv_until != 0 && !before(adv_until)) {
        a->stop();
        if (adv == Adv::Pair) {
            ui::set_notice("蓝牙键盘配对已超时：没有电脑连上", true);
            events::emit("ble://event/pair_timeout");
        } else {
            ui::set_notice("蓝牙广播已结束");
            events::emit("ble://event/advertise_done");
        }
        adv = Adv::None;
        adv_until = 0;
    }
    bool have_computers;
    {
        std::lock_guard<std::mutex> guard(lock);
        have_computers = !computers.empty();
    }
    if (adv == Adv::None && have_computers && hid_peers == 0) {
        start_keyboard_advertising(false);
        adv = Adv::Reconnect;
    } else if (adv == Adv::Reconnect && (hid_peers > 0 || !have_computers)) {
        a->stop();
        adv = Adv::None;
    } else if ((adv == Adv::Reconnect || adv == Adv::Pair) && hid_peers == 0 && !a->isAdvertising()) {
        a->start();  // a connection that did not pair ends connectable advertising
    }
}

String adv_text() {
    switch (adv) {
        case Adv::Beacon: return before(adv_until) ? "广播中（还剩 " + String((adv_until - millis()) / 1000) + " 秒）" : String("广播中");
        case Adv::Pair: return "等待电脑配对（还剩 " + String((adv_until - millis()) / 1000) + " 秒）";
        case Adv::Reconnect: return "等待已配对的电脑连接";
        default: return "无";
    }
}

std::vector<uint8_t> parse_hex(const String& text, bool& ok) {
    std::vector<uint8_t> out;
    ok = true;
    int high = -1;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == ':' || c == '-') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else {
            ok = false;
            return {};
        }
        if (high < 0) high = v;
        else {
            out.push_back(static_cast<uint8_t>(high << 4 | v));
            high = -1;
        }
    }
    if (high >= 0) ok = false;
    return out;
}

// ------------------------------------------------------------------ central

// Why a central connection ended (0x08 supervision timeout = radio, 0x13 the
// device hung up, 0x16 we did), for the runtime log.
class ClientWatch : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override { runtime_event("ble", "central connected " + String(c->getPeerAddress().toString().c_str())); }
    void onDisconnect(NimBLEClient* c, int reason) override {
        runtime_event("ble", "central disconnected " + String(c->getPeerAddress().toString().c_str()) + " reason=0x" + String(reason & 0xFF, HEX) +
                                 " (" + String(reason) + ")");
    }
};
ClientWatch client_watch;

NimBLEClient* client_for(const String& address, String& error) {
    uint8_t mac[6];
    if (!parse_mac(address.c_str(), mac)) {
        error = "地址格式应为 aa:bb:cc:dd:ee:ff（先 read ble://scan 找设备）";
        return nullptr;
    }
    int known_type = -1;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (const Device* d = find_device(mac)) known_type = d->type;
    }
    const std::string text = mac_text(mac).c_str();
    for (uint8_t type : {static_cast<uint8_t>(known_type < 0 ? BLE_ADDR_PUBLIC : known_type), static_cast<uint8_t>(BLE_ADDR_RANDOM)}) {
        const NimBLEAddress peer(text, type);
        NimBLEClient* c = NimBLEDevice::getClientByPeerAddress(peer);
        if (c && c->isConnected()) return c;
    }
    if (NimBLEDevice::getConnectedClients().size() >= kMaxClients) {
        error = "已经连着 " + String(kMaxClients) + " 个设备，先断开一个（session disconnect ble://device/地址）";
        return nullptr;
    }
    connecting = true;
    NimBLEDevice::getScan()->stop();  // a scan in progress makes the connection fail
    std::vector<uint8_t> types;
    if (known_type >= 0) types.push_back(known_type);
    else types = {BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM};
    NimBLEClient* client = nullptr;
    for (uint8_t type : types) {
        const NimBLEAddress peer(text, type);
        NimBLEClient* c = NimBLEDevice::getClientByPeerAddress(peer);
        if (!c) c = NimBLEDevice::createClient(peer);
        if (!c) break;
        c->setConnectTimeout(kConnectTimeoutMs);
        c->setClientCallbacks(&client_watch, false);
        if (c->connect(peer)) {
            client = c;
            break;
        }
        NimBLEDevice::deleteClient(c);
    }
    connecting = false;
    if (!client) error = "连不上 " + address + "（不在附近、不可连接，或已被别的设备连着）";
    return client;
}

NimBLERemoteCharacteristic* characteristic_for(const String& address, const String& service, const String& characteristic, String& error) {
    NimBLEClient* c = client_for(address, error);
    if (!c) return nullptr;
    NimBLERemoteService* s = c->getService(NimBLEUUID(std::string(service.c_str())));
    if (!s) {
        error = "设备 " + address + " 上没有服务 " + service + "（read ble://device/" + address + "/services 看有哪些）";
        return nullptr;
    }
    NimBLERemoteCharacteristic* ch = s->getCharacteristic(NimBLEUUID(std::string(characteristic.c_str())));
    if (!ch) error = "服务 " + service + " 里没有特征 " + characteristic;
    return ch;
}

// Text only when the bytes are valid UTF-8 without control characters: any
// other byte would make the reply (JSON) invalid.
bool is_text(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length;) {
        const uint8_t c = data[i];
        size_t more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
        if (more == 9 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) return false;
        if (i + more >= length) return false;  // truncated sequence
        for (size_t k = 1; k <= more; ++k)
            if ((data[i + k] & 0xC0) != 0x80) return false;
        i += more + 1;
    }
    return true;
}

String value_text(const uint8_t* data, size_t length) {
    String out = "长度 " + String(length) + " 字节";
    if (length == 0) return out;
    out += "\nhex:";
    for (size_t i = 0; i < length && i < 64; ++i) {
        char b[4];
        snprintf(b, sizeof b, " %02x", data[i]);
        out += b;
    }
    if (length > 64) out += " …";
    if (is_text(data, length)) out += "\n文本: " + String(reinterpret_cast<const char*>(data), length);
    if (length == 1) out += "\n数值: " + String(data[0]);
    if (length == 2) out += "\n数值（小端）: " + String(data[0] | data[1] << 8) + "（有符号 " + String(static_cast<int16_t>(data[0] | data[1] << 8)) + "）";
    if (length == 4) {
        const uint32_t v = data[0] | data[1] << 8 | data[2] << 16 | static_cast<uint32_t>(data[3]) << 24;
        out += "\n数值（小端）: " + String(v) + "（有符号 " + String(static_cast<int32_t>(v)) + "）";
    }
    return out;
}

String key_of(const String& address, const String& service, const String& characteristic) {
    String key = address + "/" + service + "/" + characteristic;
    key.toLowerCase();
    return key;
}

// ----------------------------------------------------------------- keyboard

bool key_for(char c, uint8_t& modifier, uint8_t& key) {
    // US layout: the character without Shift, with Shift, and the key.
    struct Punct {
        char plain;
        char shifted;
        uint8_t key;
    };
    static const Punct kPunct[] = {{'-', '_', 0x2D}, {'=', '+', 0x2E},  {'[', '{', 0x2F}, {']', '}', 0x30},
                                   {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35},
                                   {',', '<', 0x36}, {'.', '>', 0x37},  {'/', '?', 0x38}};
    static const char kShiftedDigits[] = "!@#$%^&*()";  // Shift + 1 … 0
    modifier = 0;
    if (c >= 'a' && c <= 'z') {
        key = 0x04 + (c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        key = 0x04 + (c - 'A');
        modifier = 0x02;
        return true;
    }
    if (c >= '1' && c <= '9') {
        key = 0x1E + (c - '1');
        return true;
    }
    switch (c) {
        case '0': key = 0x27; return true;
        case '\n': key = 0x28; return true;
        case '\t': key = 0x2B; return true;
        case ' ': key = 0x2C; return true;
        default: break;
    }
    for (const auto& p : kPunct) {
        if (c == p.plain || c == p.shifted) {
            key = p.key;
            modifier = c == p.shifted ? 0x02 : 0;
            return true;
        }
    }
    if (c != '\0') {
        if (const char* d = strchr(kShiftedDigits, c)) {
            key = 0x1E + (d - kShiftedDigits);
            modifier = 0x02;
            return true;
        }
    }
    return false;
}

bool named_key(String name, uint8_t& key) {
    name.toLowerCase();
    struct Named {
        const char* name;
        uint8_t key;
    };
    static const Named kNamed[] = {{"enter", 0x28},    {"return", 0x28},  {"esc", 0x29},     {"escape", 0x29},
                                   {"backspace", 0x2A}, {"tab", 0x2B},     {"space", 0x2C},   {"delete", 0x4C},
                                   {"right", 0x4F},     {"left", 0x50},    {"down", 0x51},    {"up", 0x52},
                                   {"home", 0x4A},      {"end", 0x4D},     {"pageup", 0x4B},  {"pagedown", 0x4E}};
    for (const auto& n : kNamed) {
        if (name == n.name) {
            key = n.key;
            return true;
        }
    }
    if (name.length() >= 2 && name.length() <= 3 && name[0] == 'f') {
        const int f = name.substring(1).toInt();
        if (f >= 1 && f <= 12) {
            key = 0x3A + f - 1;
            return true;
        }
    }
    return false;
}

// Paired, encrypted computers that also subscribed to key reports: a link
// whose host never subscribed (no HID driver took it) would swallow keys.
std::vector<uint16_t> receivers() {
    std::vector<uint16_t> handles;
    std::lock_guard<std::mutex> guard(lock);
    for (uint8_t i = 0; i < server->getConnectedCount(); ++i) {
        const NimBLEConnInfo info = server->getPeerInfo(i);
        const uint16_t h = info.getConnHandle();
        if (info.isEncrypted() && info.isBonded() && known_computer(info.getIdAddress()) &&
            std::find(subscribed.begin(), subscribed.end(), h) != subscribed.end())
            handles.push_back(h);
    }
    return handles;
}

void send_report(uint8_t modifier, uint8_t key) {
    const uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    for (uint16_t h : receivers()) keyboard_input->notify(report, sizeof report, h);
}

void tap(uint8_t modifier, uint8_t key) {
    send_report(modifier, key);
    delay(12);
    send_report(0, 0);
    delay(12);
}

class KeyReports : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t value) override {
        std::lock_guard<std::mutex> guard(lock);
        forget_subscription(info.getConnHandle());
        if (value) subscribed.push_back(info.getConnHandle());
    }
};
KeyReports key_reports;

// Why there is nobody to type to, or "" when there is.
String no_receiver() {
    hid_peers = count_hosts();
    if (hid_peers > 0 && receivers().empty())
        return "电脑连上了，但没有把它当键盘接收（没有订阅按键）：在电脑的蓝牙设置里删掉 FeiyuOS 重新配对；"
               "Linux 还要确认 uhid 模块能加载（内核升级后没重启时加载不了）";
    if (hid_peers > 0) return "";
    std::lock_guard<std::mutex> guard(lock);
    return computers.empty() ? String("还没有配对过电脑：先说「蓝牙键盘配对」，再在电脑的蓝牙设置里选 FeiyuOS")
                             : String("已配对的电脑现在没有连着：打开电脑的蓝牙，稍等它自动连上");
}

}  // namespace

// ------------------------------------------------------------------- public

String begin() {
    if (!config::get().ble.enabled) {
        off_reason = "未开启";
        return off_reason;
    }
    const uint32_t heap_before = ESP.getFreeHeap();
    devices = new (std::nothrow) Device[kMaxDevices];
    subscriptions = new (std::nothrow) Subscription[kMaxSubscriptions];
    if (!devices || !subscriptions || !NimBLEDevice::init(kName)) {
        delete[] devices;
        delete[] subscriptions;
        devices = nullptr;
        subscriptions = nullptr;
        off_reason = "初始化失败（内存不足？）";
        return off_reason;
    }
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM (no keys to type), secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&listener, true);  // every advertisement: RSSI keeps changing
    scan->setActiveScan(true);                // names often come in the scan response
    scan->setInterval(100);
    scan->setWindow(30);                      // 30 % of the radio: Wi-Fi keeps the rest
    scan->setMaxResults(0);                   // our table only, nothing kept inside NimBLE

    server = NimBLEDevice::createServer();
    server->setCallbacks(&host, false);
    server->advertiseOnDisconnect(false);
    hid = new NimBLEHIDDevice(server);
    hid->setManufacturer(kName);
    hid->setPnp(0x02, 0x303A, 0x4001, 0x0100);
    hid->setHidInfo(0x00, 0x01);
    hid->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof kReportMap);
    keyboard_input = hid->getInputReport(1);
    keyboard_input->setCallbacks(&key_reports);
    hid->getOutputReport(1);  // the LED report the map declares
    hid->setBatteryLevel(100);
    server->start();

    {
        std::lock_guard<std::mutex> guard(lock);
        for (int i = 0; i < NimBLEDevice::getNumBonds(); ++i) computers.push_back(NimBLEDevice::getBondedAddress(i));
    }
    started = true;
    off_reason = "";
    heap_taken = heap_before > ESP.getFreeHeap() ? heap_before - ESP.getFreeHeap() : 0;
    arena::note("ble", heap_taken);
    runtime_event("ble", "on, " + String(heap_taken / 1024.0f, 1) + " KB, " + String(computers.size()) + " bonded");
    return "";
}

void loop() {
    if (!started) return;
    update_scan();
    update_advertising();
    flush_log();
    if (logging && !before(log_until)) {
        logging = false;
        flush_log(true);
        ui::set_notice("蓝牙记录完成：" + String(logged_lines) + " 条，存在 " + log_path);
        events::emit("ble://event/log_done", "{\"lines\":" + String(logged_lines) + "}");
    }
    const int hosts = hid_peers;
    if (hosts != shown_hosts) {
        if (hosts > shown_hosts) ui::set_notice("电脑已连上蓝牙键盘");
        else if (hosts == 0) ui::set_notice("电脑断开了蓝牙键盘");
        events::emit(hosts > shown_hosts ? "ble://event/connected" : "ble://event/disconnected");
        shown_hosts = hosts;
    }
}

bool running() { return started; }
bool enabled() { return config::get().ble.enabled; }

String set_enabled(bool on) {
    config::get().ble.enabled = on;
    if (!config::save_value("ble", "enabled", cJSON_CreateBool(on))) return "保存失败";
    if (on == started) return on ? "蓝牙已经开着" : "蓝牙已经关着";
    return on ? "蓝牙已设为开启，重启后生效（开机时先占好约 50 KB 内存）" : "蓝牙已设为关闭，重启后释放内存";
}

String status_text() {
    if (!started) return "蓝牙：关（" + off_reason + "；设置 → 蓝牙，改完重启）";
    int count = 0, strongest = 0;
    nearby(count, strongest);
    String out = "蓝牙：开，占用约 " + String(heap_taken / 1024) + " KB 内存";
    out += "\n附近（10 秒内）：" + String(count) + " 个设备" + (count ? "，最强 " + String(strongest) + " dBm" : "");
    out += "\n扫描：" + String(NimBLEDevice::getScan()->isScanning() ? "进行中" : "空闲");
    if (before(log_until)) out += "；记录到 " + log_path + "（还剩 " + String((log_until - millis()) / 1000) + " 秒）";
    out += "\n广播：" + adv_text();
    out += "\n" + hid_status();
    const auto clients = NimBLEDevice::getConnectedClients();
    out += "\n已连接的设备：" + String(clients.empty() ? "无" : "");
    for (auto* c : clients) out += "\n  " + String(c->getPeerAddress().toString().c_str());
    return out;
}

String scan(uint32_t seconds) {
    if (!started) return off_text();
    seconds = constrain(seconds, 1u, 30u);
    const uint32_t from = millis();
    scan_until = from + seconds * 1000;
    while (before(scan_until)) delay(100);  // loop() keeps the radio listening until then
    return list_text(from);
}

String devices_text() {
    if (!started) return off_text();
    return list_text(0);
}

String device_text(const String& address) {
    if (!started) return off_text();
    uint8_t mac[6];
    if (!parse_mac(address.c_str(), mac)) return "地址格式应为 aa:bb:cc:dd:ee:ff";
    Device d;
    {
        std::lock_guard<std::mutex> guard(lock);
        const Device* found = find_device(mac);
        if (!found) return "最近没有听到 " + address + "（先 read ble://scan）";
        d = *found;
    }
    const uint32_t now = millis();
    String out = format_device(d, 1, now).substring(3);
    out += "\n地址类型：" + String(d.type == BLE_ADDR_PUBLIC ? "公共" : "随机（手机等会定期更换）");
    out += "\n第一次听到：" + String((now - d.first_ms) / 1000) + " 秒前";
    if (d.has_tx) out += "\n发射功率：" + String(d.tx_power) + " dBm";
    if (d.appearance) out += "\n外观代码：0x" + String(d.appearance, HEX);
    for (auto* c : NimBLEDevice::getConnectedClients()) {
        if (String(c->getPeerAddress().toString().c_str()).equalsIgnoreCase(address)) out += "\n状态：已连接";
    }
    return out;
}

String scan_log(uint32_t seconds) {
    if (!started) return off_text();
    if (seconds == 0) {
        if (!before(log_until)) return "没有在记录";
        log_until = 0;
        logging = false;
        flush_log(true);
        return "已停止记录：" + String(logged_lines) + " 条，存在 " + log_path;
    }
    seconds = constrain(seconds, 10u, 3600u);
    if (!before(log_until)) {
        struct tm t;
        char name[48];
        if (getLocalTime(&t, 0)) strftime(name, sizeof name, "/vat/ble/scan-%Y%m%d-%H%M%S.jsonl", &t);
        else snprintf(name, sizeof name, "/vat/ble/scan-boot%lu.jsonl", static_cast<unsigned long>(millis() / 1000));
        log_path = name;
        storage::ensure_parent(log_path);
        logged_lines = 0;
    }
    logging = true;
    log_until = millis() + seconds * 1000;
    return "开始记录 " + String(seconds) + " 秒：每条广播写进 " + log_path + "（JSON 行：时间、地址、RSSI、名字、厂商、服务）；记完屏幕上会提示";
}

void want_scan(bool on) { stream_readers = std::max(0, stream_readers + (on ? 1 : -1)); }

bool nearby(int& count, int& strongest) {
    count = 0;
    strongest = -127;
    if (!started) return false;
    std::unique_lock<std::mutex> guard(lock, std::try_to_lock);
    if (!guard.owns_lock()) return false;
    const uint32_t now = millis();
    for (int i = 0; i < kMaxDevices; ++i) {
        if (devices[i].used && now - devices[i].last_ms < kRecentMs) {
            ++count;
            strongest = std::max<int>(strongest, devices[i].rssi);
        }
    }
    if (count == 0) strongest = 0;
    return true;
}

bool rssi(const String& address, int& dbm) {
    uint8_t mac[6];
    if (!started || !parse_mac(address.c_str(), mac)) return false;
    std::unique_lock<std::mutex> guard(lock, std::try_to_lock);
    if (!guard.owns_lock()) return false;
    const Device* d = find_device(mac);
    if (!d || millis() - d->last_ms > kRssiFreshMs) return false;
    dbm = d->rssi;
    return true;
}

String advertise(const String& spec) {
    if (!started) return off_text();
    if (adv == Adv::Pair) return "正在等电脑配对，配好或超时后再广播";
    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    uint32_t seconds = 0;
    String what;
    cJSON* root = spec.startsWith("{") ? cJSON_Parse(spec.c_str()) : nullptr;
    if (spec.startsWith("{") && !root) return "广播内容应为 JSON，如 {\"name\":\"FeiyuOS\"} 或 {\"ibeacon\":{\"uuid\":\"…\",\"major\":1,\"minor\":2}}";
    const String name = root ? json_str(root, "name") : spec;
    if (root) {
        seconds = static_cast<uint32_t>(std::max(0.0, cJSON_GetNumberValue(cJSON_GetObjectItem(root, "seconds"))));
        if (cJSON* beacon = cJSON_GetObjectItem(root, "ibeacon")) {
            const String uuid = json_str(beacon, "uuid");
            if (uuid.length() != 36) {
                cJSON_Delete(root);
                return "iBeacon 需要 36 位的 uuid（如 e2c56db5-dffb-48d2-b060-d0f5a71096e0）";
            }
            NimBLEBeacon b;
            b.setManufacturerId(0x4C00);
            b.setProximityUUID(NimBLEUUID(std::string(uuid.c_str())));
            b.setMajor(static_cast<uint16_t>(cJSON_GetNumberValue(cJSON_GetObjectItem(beacon, "major"))));
            b.setMinor(static_cast<uint16_t>(cJSON_GetNumberValue(cJSON_GetObjectItem(beacon, "minor"))));
            cJSON* power = cJSON_GetObjectItem(beacon, "power");
            b.setSignalPower(cJSON_IsNumber(power) ? static_cast<int8_t>(power->valuedouble) : -59);
            data.setManufacturerData(reinterpret_cast<const uint8_t*>(&b.getData()), sizeof(NimBLEBeacon::BeaconData));
            what = "iBeacon " + uuid;
        } else if (cJSON_IsString(cJSON_GetObjectItem(root, "data"))) {
            bool ok;
            const auto bytes = parse_hex(json_str(root, "data"), ok);
            if (!ok || bytes.size() < 2) {
                cJSON_Delete(root);
                return "data 应为十六进制，前两个字节是厂商 ID（小端），如 \"ffff0102\"";
            }
            data.setManufacturerData(bytes);
            what = "厂商数据 " + String(bytes.size()) + " 字节";
        }
        cJSON_Delete(root);
    }
    if (!name.isEmpty()) {
        data.setName(name.c_str());
        what += String(what.isEmpty() ? "" : "，") + "名字「" + name + "」";
    }
    if (what.isEmpty()) return "说清广播什么：名字、厂商数据（data）或 iBeacon";
    NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
    a->stop();
    a->reset();
    if (!a->setAdvertisementData(data)) return "广播内容太长（最多 31 字节：名字和数据加起来要短）";
    a->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    a->setMinInterval(160);  // 100 ms
    a->setMaxInterval(320);
    if (!a->start()) return "广播启动失败";
    adv = Adv::Beacon;
    adv_until = seconds ? millis() + std::min<uint32_t>(seconds, 86400) * 1000 : 0;
    return "开始广播：" + what + (seconds ? "，" + String(seconds) + " 秒后停" : "，直到停止（write ble://advertise 内容 off）") +
           (computers.empty() ? "" : "；广播期间已配对的电脑连不上键盘");
}

String advertise_stop() {
    if (!started) return off_text();
    if (adv != Adv::Beacon) return "没有在广播";
    NimBLEDevice::getAdvertising()->stop();
    adv = Adv::None;
    adv_until = 0;
    return "已停止广播";
}

String connect(const String& address) {
    if (!started) return off_text();
    String error;
    NimBLEClient* c = client_for(address, error);
    if (!c) return error;
    return "已连接 " + address + "。看它有哪些数据：read ble://device/" + address + "/services";
}

String disconnect(const String& address) {
    if (!started) return off_text();
    bool found = false;
    for (auto* c : NimBLEDevice::getConnectedClients()) {
        if (String(c->getPeerAddress().toString().c_str()).equalsIgnoreCase(address)) {
            c->disconnect();
            NimBLEDevice::deleteClient(c);
            found = true;
        }
    }
    String prefix = address;
    prefix.toLowerCase();
    {
        std::lock_guard<std::mutex> guard(lock);
        for (int i = 0; i < kMaxSubscriptions; ++i)
            if (subscriptions[i].used && String(subscriptions[i].key).startsWith(prefix)) subscriptions[i] = Subscription();
    }
    return found ? "已断开 " + address : address + " 没有连着";
}

String services(const String& address) {
    if (!started) return off_text();
    String error;
    NimBLEClient* c = client_for(address, error);
    if (!c) return error;
    // discoverAttributes() failed outright against a BlueZ peer (descriptor
    // discovery); service by service works, with one retry for a service that
    // came back empty (seen on 128-bit services, 2026-11-08).
    String out = address + " 的服务和特征（读：read ble://gatt/" + address + "/服务/特征）：";
    for (auto* s : c->getServices(true)) {
        if (s->getCharacteristics(true).empty()) s->getCharacteristics(true);
        const String su = s->getUUID().toString().c_str();
        const char* sn = service_name(su);
        out += "\n服务 " + su + (sn ? "（" + String(sn) + "）" : "");
        if (s->getCharacteristics(false).empty()) out += "\n  （没列出特征：知道 UUID 的话可以直接 read ble://gatt/…）";
        for (auto* ch : s->getCharacteristics(false)) {
            String props;
            if (ch->canRead()) props += " 读";
            if (ch->canWrite()) props += " 写";
            if (ch->canWriteNoResponse()) props += " 无应答写";
            if (ch->canNotify()) props += " 通知";
            if (ch->canIndicate()) props += " 指示";
            out += "\n  特征 " + String(ch->getUUID().toString().c_str()) + " [" + props.substring(1) + "]";
            if (out.length() > 3000) return out + "\n…（太多，只列到这里）";
        }
    }
    return out;
}

String gatt_read(const String& address, const String& service, const String& characteristic) {
    if (!started) return off_text();
    String error;
    NimBLERemoteCharacteristic* ch = characteristic_for(address, service, characteristic, error);
    if (!ch) return error;
    if (!ch->canRead()) return "这个特征不能读" + String(ch->canNotify() ? "，只能订阅通知" : "");
    const NimBLEAttValue v = ch->readValue();
    return value_text(v.data(), v.size());
}

String gatt_write(const String& address, const String& service, const String& characteristic, const String& value) {
    if (!started) return off_text();
    std::vector<uint8_t> bytes;
    if (value.startsWith("hex:")) {
        bool ok;
        bytes = parse_hex(value.substring(4), ok);
        if (!ok) return "hex: 后面应为十六进制字节，如 hex:01 a0";
    } else {
        bytes.assign(value.c_str(), value.c_str() + value.length());
    }
    if (bytes.empty() || bytes.size() > 244) return "写入内容应为 1–244 字节";
    String error;
    NimBLERemoteCharacteristic* ch = characteristic_for(address, service, characteristic, error);
    if (!ch) return error;
    const bool response = ch->canWrite();
    if (!response && !ch->canWriteNoResponse()) return "这个特征不能写";
    if (!ch->writeValue(bytes.data(), bytes.size(), response)) return "写入失败（设备拒绝或断开了）";
    return "已写入 " + String(bytes.size()) + " 字节" + (response ? "，设备已确认" : "");
}

String subscribe(const String& address, const String& service, const String& characteristic, bool on) {
    if (!started) return off_text();
    const String key = key_of(address, service, characteristic);
    int slot = -1;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (int i = 0; i < kMaxSubscriptions; ++i)
            if (subscriptions[i].used && key == subscriptions[i].key) slot = i;
        if (on && slot < 0) {
            for (int i = 0; i < kMaxSubscriptions && slot < 0; ++i)
                if (!subscriptions[i].used) slot = i;
            if (slot < 0) return "最多同时订阅 " + String(kMaxSubscriptions) + " 个特征，先取消一个";
        }
    }
    if (!on && slot < 0) return "没有订阅 " + key;
    String error;
    NimBLERemoteCharacteristic* ch = characteristic_for(address, service, characteristic, error);
    if (!ch) return error;
    if (!on) {
        ch->unsubscribe();
        std::lock_guard<std::mutex> guard(lock);
        subscriptions[slot] = Subscription();
        return "已取消订阅";
    }
    if (!ch->canNotify() && !ch->canIndicate()) return "这个特征不能订阅（没有通知 / 指示）";
    {
        std::lock_guard<std::mutex> guard(lock);
        subscriptions[slot] = Subscription();
        subscriptions[slot].used = true;
        strncpy(subscriptions[slot].key, key.c_str(), sizeof(subscriptions[slot].key) - 1);
    }
    const bool ok = ch->subscribe(ch->canNotify(), [slot](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
        std::lock_guard<std::mutex> guard(lock);
        Subscription& s = subscriptions[slot];
        if (!s.used) return;
        s.length = std::min(length, kNotifyBytes);
        memcpy(s.bytes, data, s.length);
        s.at_ms = millis();
    });
    if (!ok) {
        std::lock_guard<std::mutex> guard(lock);
        subscriptions[slot] = Subscription();
        return "订阅失败";
    }
    return "已订阅。程序里这样取：{\"stream\":\"ble://notify/" + key + "\",\"as\":\"n\"}，字段 n.len、n.b0…n.b6";
}

String notify_text(const String& key) {
    if (!started) return off_text();
    String k = key;
    k.toLowerCase();
    Subscription s;
    {
        std::lock_guard<std::mutex> guard(lock);
        bool found = false;
        for (int i = 0; i < kMaxSubscriptions && !found; ++i) {
            if (subscriptions[i].used && k == subscriptions[i].key) {
                s = subscriptions[i];
                found = true;
            }
        }
        if (!found) return "没有订阅 " + key + "（先 write ble://notify/" + key + " 内容 on）";
    }
    if (s.at_ms == 0) return "已订阅，还没收到通知";
    return "最新通知（" + String((millis() - s.at_ms) / 1000) + " 秒前）：" + value_text(s.bytes, s.length);
}

bool notified(const String& key, uint8_t* bytes, size_t capacity, size_t& length) {
    if (!started) return false;
    String k = key;
    k.toLowerCase();
    std::unique_lock<std::mutex> guard(lock, std::try_to_lock);
    if (!guard.owns_lock()) return false;
    for (int i = 0; i < kMaxSubscriptions; ++i) {
        const Subscription& s = subscriptions[i];
        if (!s.used || k != s.key || s.at_ms == 0) continue;
        length = std::min(s.length, capacity);
        memcpy(bytes, s.bytes, length);
        return true;
    }
    return false;
}

String hid_pair(uint32_t seconds) {
    if (!started) return off_text();
    seconds = constrain(seconds == 0 ? 60u : seconds, 20u, 180u);
    adv = Adv::Pair;
    adv_until = millis() + seconds * 1000;
    start_keyboard_advertising(true);
    return "键盘配对已打开 " + String(seconds) + " 秒：在电脑的蓝牙设置里选「" + String(kName) +
           "」。配好或超时屏幕上都会提示；之后电脑每次打开蓝牙会自动连上。这段时间里附近任何电脑都能配对，配完即关闭。";
}

String hid_type(const String& text) {
    if (!started) return off_text();
    const String missing = no_receiver();
    if (!missing.isEmpty()) return missing;
    int typed = 0, skipped = 0;
    for (size_t i = 0; i < text.length() && i < 1000; ++i) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        if (c >= 0x80) {  // a UTF-8 character: keyboards only send keys, the computer's input method makes Chinese
            if ((c & 0xC0) != 0x80) ++skipped;
            continue;
        }
        uint8_t modifier, key;
        if (!key_for(static_cast<char>(c), modifier, key)) {
            ++skipped;
            continue;
        }
        tap(modifier, key);
        ++typed;
    }
    String out = "已输入 " + String(typed) + " 个字符";
    if (skipped) out += "；跳过 " + String(skipped) + " 个（键盘只能打英文、数字和符号，中文要靠电脑上的输入法）";
    return out;
}

String hid_key(const String& combo) {
    if (!started) return off_text();
    const String missing = no_receiver();
    if (!missing.isEmpty()) return missing;
    uint8_t modifier = 0, key = 0;
    int from = 0;
    String c = combo;
    c.trim();
    while (from <= static_cast<int>(c.length())) {
        int plus = c.indexOf('+', from);
        if (plus == from && plus == static_cast<int>(c.length()) - 1) plus = -1;  // "ctrl++"
        String part = plus < 0 ? c.substring(from) : c.substring(from, plus);
        part.trim();
        String lower = part;
        lower.toLowerCase();
        if (lower == "ctrl" || lower == "control") modifier |= 0x01;
        else if (lower == "shift") modifier |= 0x02;
        else if (lower == "alt" || lower == "option") modifier |= 0x04;
        else if (lower == "cmd" || lower == "command" || lower == "win" || lower == "gui" || lower == "super") modifier |= 0x08;
        else if (named_key(lower, key)) {
        } else if (part.length() == 1) {
            uint8_t extra;
            if (!key_for(part[0], extra, key)) return "不认识的键：" + part;
            modifier |= extra;
        } else {
            return "不认识的键：" + part + "（可以用 enter、tab、esc、backspace、space、上下左右、f1–f12，加 ctrl/shift/alt/cmd）";
        }
        if (plus < 0) break;
        from = plus + 1;
    }
    if (!key) return "要按哪个键？如 enter、cmd+space、ctrl+c";
    tap(modifier, key);
    return "已按下 " + combo;
}

String hid_forget() {
    if (!started) return off_text();
    std::vector<NimBLEAddress> forget;
    {
        std::lock_guard<std::mutex> guard(lock);
        forget.swap(computers);
    }
    for (const auto& a : forget) NimBLEDevice::deleteBond(a);
    while (server->getConnectedCount() > 0) {
        if (!server->disconnect(server->getPeerInfo(0))) break;
        delay(50);
    }
    hid_peers = 0;
    if (adv == Adv::Reconnect) {
        NimBLEDevice::getAdvertising()->stop();
        adv = Adv::None;
    }
    return "已忘记 " + String(forget.size()) + " 个配对（电脑那边也删掉「" + String(kName) + "」）";
}

String hid_status() {
    if (!started) return off_text();
    size_t paired;
    {
        std::lock_guard<std::mutex> guard(lock);
        paired = computers.size();
    }
    hid_peers = count_hosts();
    return "键盘：已配对 " + String(paired) + " 台，已连接 " + String(hid_peers) + " 台" +
           (adv == Adv::Pair ? "；" + adv_text() : "");
}

}  // namespace vat::ble
