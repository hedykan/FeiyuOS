#include "net.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "common.h"
#include "storage.h"

namespace vat::net {
namespace {

WifiState wifi_state = WifiState::Off;

// mbedTLS needs two ~16.7 KB contiguous buffers per connection.  Long-running
// use fragments the heap until no such block is left even with 90 KB free, so
// two blocks are claimed while the heap is still clean and handed back only
// for the duration of a TLS connection.
constexpr std::size_t kTlsReserveBytes = 17920;
void* tls_reserve[2] = {nullptr, nullptr};

void reserve_blocks() {
    for (auto& block : tls_reserve) {
        if (!block) block = heap_caps_malloc(kTlsReserveBytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
}

void release_blocks() {
    for (auto& block : tls_reserve) {
        free(block);
        block = nullptr;
    }
}
String setup_ap_name;
constexpr uint32_t kSampleRate = 16000;

bool split_url(const String& url, String& host, uint16_t& port) {
    if (!url.startsWith("https://")) return false;
    const int start = 8;
    int end = url.indexOf('/', start);
    if (end < 0) end = url.length();
    host = url.substring(start, end);
    port = 443;
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
                    uint32_t timeout_ms, const String* save_path = nullptr) {
    HttpResult result;
    String host;
    uint16_t port;
    if (!split_url(url, host, port)) {
        result.status = -1;
        result.error = "invalid url";
        return result;
    }
    if (cancel::requested()) {
        result.status = -2;
        result.error = "cancelled";
        return result;
    }
    release_blocks();
    struct Rearm {
        ~Rearm() { reserve_blocks(); }
    } rearm;  // reclaim the reserve once the connection is gone
    NetworkClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(timeout_ms / 1000 + 1);
    tls.setHandshakeTimeout(12);
    if (!tls.connect(host.c_str(), port, 12000)) {
        char reason[96] = {};
        const int code = tls.lastError(reason, sizeof(reason));
        result.status = -3;
        result.error = "tls connect failed " + String(code) + " " + reason + " heap=" + ESP.getFreeHeap() +
                       " block=" + ESP.getMaxAllocHeap() + " wifi=" + WiFi.status();
        runtime_event("tls_fail", host + " " + result.error);
        return result;
    }
    cancel::set_socket(tls.fd());
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(timeout_ms);
    http.setReuse(false);
    if (!http.begin(tls, url)) {
        cancel::clear_socket();
        result.status = -1;
        result.error = "http begin failed";
        return result;
    }
    if (!api_key.isEmpty()) http.addHeader("Authorization", "Bearer " + api_key);
    http.addHeader("Content-Type", "application/json");
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
    if (result.status > 0 && !cancel::requested() && save_path && result.status == 200) {
        // Large bodies go straight to storage in small blocks.
        storage::remove(*save_path);
        File file = storage::ensure_parent(*save_path) ? storage::fs().open(*save_path, FILE_WRITE) : File();
        if (!file || http.writeToStream(&file) < 0) result.error = "response not saved";
        if (file) file.close();
        storage::changed(*save_path);  // written through fs(): drop the cached size
    } else if (result.status > 0 && !cancel::requested()) {
        result.body = http.getString();
    } else {
        result.error = HTTPClient::errorToString(result.status);
    }
    http.end();
    cancel::clear_socket();
    tls.stop();
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

void release_reserve() { release_blocks(); }
void restore_reserve() { reserve_blocks(); }

bool connect_station(const String& ssid, const String& password, uint32_t timeout_ms) {
    if (ssid.isEmpty()) return false;
    wifi_state = WifiState::Connecting;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("vat-cardputer");
    WiFi.begin(ssid.c_str(), password.c_str());
    const unsigned long deadline = millis() + timeout_ms;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        reserve_blocks();
        wifi_state = WifiState::Connected;
        return true;
    }
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
String ap_name() { return setup_ap_name; }

HttpResult post_json(const String& url, const String& api_key, const String& body,
                     uint32_t timeout_ms, int attempts) {
    HttpResult result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        result = exchange("POST", url, api_key, &body, nullptr, 0, timeout_ms);
        runtime_event("http_post", url.substring(8, 40) + " status=" + result.status +
                                       " attempt=" + attempt + " bytes=" + result.body.length());
        if (!retryable(result) || attempt == attempts) break;
        delay(400 * attempt);
    }
    return result;
}

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
