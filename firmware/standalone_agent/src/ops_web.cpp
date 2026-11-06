#include "ops_web.h"

#include <cJSON.h>

#include <functional>
#include <vector>

#include "common.h"
#include "config.h"
#include "context.h"
#include "net.h"
#include "ops_ssh.h"
#include "storage.h"

namespace vat::web {
namespace {

constexpr char kSearchUrl[] = "https://api.deepseek.com/v1/tools/search";
constexpr char kFetchUrl[] = "https://api.deepseek.com/v1/tools/fetch";
constexpr char kResponse[] = "/vat/cache/web.json";
constexpr char kPage[] = "/vat/cache/web-page.md";
constexpr std::size_t kPageMax = 262144;
constexpr std::size_t kPageHead = 4000;  // bytes of the page shown in the conversation

// Streams a JSON document from a file.  For every string value whose key is
// wanted, the decoded text is handed to `sink` in pieces, with the depth of
// the object holding it; `object_start` fires on each '{' with its depth.
// Only what the callers need: no numbers, no validation beyond structure.
class JsonScan {
public:
    using Sink = std::function<void(const String& key, int depth, const char* text, std::size_t n)>;
    using ObjectStart = std::function<void(int depth)>;

    JsonScan(File& file, Sink sink, ObjectStart object_start)
        : file_(file), sink_(std::move(sink)), object_start_(std::move(object_start)) {}

    void run(const std::function<bool(const String& key)>& wanted) {
        int c;
        bool expecting_value = false;  // just saw ':' in an object
        String key;
        while ((c = next()) >= 0) {
            if (c == '{') {
                ++depth_;
                if (object_start_) object_start_(depth_);
                expecting_value = false;
            } else if (c == '}') {
                --depth_;
                expecting_value = false;
            } else if (c == '[' || c == ']' || c == ',') {
                expecting_value = false;
            } else if (c == ':') {
                expecting_value = true;
            } else if (c == '"') {
                if (expecting_value) {
                    const bool keep = wanted(key);
                    read_string(keep ? &key : nullptr);
                    expecting_value = false;
                } else {
                    key = "";
                    read_string(nullptr, &key);
                }
            }
        }
        flush();
    }

private:
    int next() {
        if (pos_ >= len_) {
            const int n = file_.read(buffer_, sizeof(buffer_));
            if (n <= 0) return -1;
            len_ = n;
            pos_ = 0;
        }
        return buffer_[pos_++];
    }

    int hex4() {
        int value = 0;
        for (int i = 0; i < 4; ++i) {
            const int c = next();
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else return -1;
        }
        return value;
    }

    void emit(const char* bytes, std::size_t n) {
        if (!target_) return;
        for (std::size_t i = 0; i < n; ++i) {
            out_[out_len_++] = bytes[i];
            if (out_len_ == sizeof(out_)) flush();
        }
    }

    void emit_codepoint(uint32_t cp) {
        char bytes[4];
        std::size_t n;
        if (cp < 0x80) { bytes[0] = cp; n = 1; }
        else if (cp < 0x800) { bytes[0] = 0xC0 | (cp >> 6); bytes[1] = 0x80 | (cp & 0x3F); n = 2; }
        else if (cp < 0x10000) {
            bytes[0] = 0xE0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 0x3F); bytes[2] = 0x80 | (cp & 0x3F); n = 3;
        } else {
            bytes[0] = 0xF0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 0x3F);
            bytes[2] = 0x80 | ((cp >> 6) & 0x3F); bytes[3] = 0x80 | (cp & 0x3F); n = 4;
        }
        emit(bytes, n);
    }

    void flush() {
        if (target_ && out_len_) sink_(*target_, depth_, out_, out_len_);
        out_len_ = 0;
    }

    // Decodes one string (opening quote already read).  `value_of` set: the
    // text goes to the sink under that key; `key_out` set: it is a key.
    void read_string(const String* value_of, String* key_out = nullptr) {
        target_ = value_of;
        int c;
        while ((c = next()) >= 0 && c != '"') {
            if (c != '\\') {
                const char byte = c;
                if (key_out) key_out->concat(byte);
                else emit(&byte, 1);
                continue;
            }
            const int e = next();
            uint32_t cp = 0;
            switch (e) {
                case 'n': cp = '\n'; break;
                case 't': cp = '\t'; break;
                case 'r': cp = '\r'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'u': {
                    const int high = hex4();
                    if (high < 0) continue;
                    cp = high;
                    if (high >= 0xD800 && high <= 0xDBFF && next() == '\\' && next() == 'u') {
                        const int low = hex4();
                        if (low >= 0xDC00 && low <= 0xDFFF) cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
                    }
                    break;
                }
                default: cp = e; break;  // \" \\ \/
            }
            if (key_out) {
                if (cp < 0x80) key_out->concat(static_cast<char>(cp));
            } else {
                emit_codepoint(cp);
            }
        }
        flush();
        target_ = nullptr;
    }

    File& file_;
    Sink sink_;
    ObjectStart object_start_;
    uint8_t buffer_[512];
    int len_ = 0;
    int pos_ = 0;
    int depth_ = 0;
    const String* target_ = nullptr;
    char out_[256];
    std::size_t out_len_ = 0;
};

String api_error(const net::HttpResult& result) {
    if (result.status == -2) return "已取消";
    if (result.status == 402) return "DeepSeek 余额不足，联网搜索不可用";
    if (result.status == 401) return "DeepSeek Key 无效";
    cJSON* body = cJSON_Parse(result.body.c_str());
    const String message = json_str(json_obj(body, "error"), "message");
    cJSON_Delete(body);
    return "联网请求失败 HTTP " + String(result.status) + " " +
           utf8_head(message.isEmpty() ? result.error : message, 120);
}

}  // namespace

String search(const String& raw_query, int count) {
    String query = raw_query;
    query.trim();
    if (query.isEmpty()) return "请给出搜索词（q=…）";
    query = utf8_head(query, 128);
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "query", query.c_str());
    cJSON_AddNumberToObject(body, "count", constrain(count, 1, 8));
    cJSON_AddBoolToObject(body, "need_content", false);
    const String json = json_print(body);
    cJSON_Delete(body);
    ssh::release();  // TLS and an SSH session never coexist (FIRMWARE_RULES §1)
    const auto result = net::post_json_to_file(kSearchUrl, config::get().deepseek_key, json, kResponse, 20000);
    if (!result.ok()) return api_error(result);

    struct Item {
        String title, site, date, summary, url;
    };
    std::vector<Item> items;
    File file = storage::fs().open(kResponse, FILE_READ);
    if (!file) return "联网搜索结果读取失败";
    // Results are objects at depth 2: {"results":[{...},{...}]}.
    JsonScan scan(
        file,
        [&](const String& key, int depth, const char* text, std::size_t n) {
            if (depth != 2 || items.empty()) return;
            Item& item = items.back();
            String* field = key == "title" ? &item.title : key == "site_name" ? &item.site
                          : key == "date" ? &item.date : key == "summary" ? &item.summary : &item.url;
            const std::size_t cap = key == "summary" ? 600 : 200;
            if (field->length() < cap) field->concat(text, std::min(n, cap - field->length()));
        },
        [&](int depth) {
            if (depth == 2) items.emplace_back();
        });
    scan.run([](const String& key) {
        return key == "title" || key == "site_name" || key == "date" || key == "summary" || key == "url";
    });
    file.close();
    storage::remove(kResponse);
    for (auto& item : items) {  // byte caps above may cut a character
        for (String* field : {&item.title, &item.site, &item.date, &item.summary, &item.url}) *field = utf8_sanitize(*field);
    }
    if (items.empty()) return "没有搜到与 “" + query + "” 相关的结果";
    String text = "联网搜索 “" + query + "” 的结果：\n";
    int n = 0;
    for (const auto& item : items) {
        text += String(++n) + ". " + item.title;
        if (!item.site.isEmpty()) text += " — " + item.site;
        if (!item.date.isEmpty()) text += "（" + item.date.substring(0, 10) + "）";
        text += "\n   " + utf8_head(item.summary, 600) + "\n   " + item.url + "\n";
    }
    return text;
}

String fetch(const String& raw_url) {
    String url = raw_url;
    url.trim();
    if (!url.startsWith("http://") && !url.startsWith("https://")) return "请给出 http(s) 网址（url=…）";
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "url", url.c_str());
    const String json = json_print(body);
    cJSON_Delete(body);
    ssh::release();
    const auto result = net::post_json_to_file(kFetchUrl, config::get().deepseek_key, json, kResponse, 30000);
    if (!result.ok()) {
        if (result.status == 403) return "该网页被抓取安全策略拒绝";
        if (result.status == 404) return "没能取到该网页的可用内容";
        return api_error(result);
    }
    File file = storage::fs().open(kResponse, FILE_READ);
    if (!file) return "网页内容读取失败";
    storage::remove(kPage);
    File page = storage::fs().open(kPage, FILE_WRITE);
    String title;
    String head;
    std::size_t written = 0;
    JsonScan scan(
        file,
        [&](const String& key, int depth, const char* text, std::size_t n) {
            if (depth != 1) return;
            if (key == "title") {
                if (title.length() < 200) title.concat(text, n);
                return;
            }
            if (written >= kPageMax) return;
            const std::size_t take = std::min(n, kPageMax - written);
            if (page) page.write(reinterpret_cast<const uint8_t*>(text), take);
            if (head.length() < kPageHead + 4) head.concat(text, std::min(take, kPageHead + 4 - head.length()));
            written += take;
        },
        nullptr);
    scan.run([](const String& key) { return key == "title" || key == "content"; });
    file.close();
    if (page) page.close();
    storage::changed(kPage);  // written through fs()
    storage::remove(kResponse);
    if (!written) {
        storage::remove(kPage);
        return "网页没有正文：" + url;
    }
    // The whole page becomes out#N with a card; without one only the head is kept.
    if (context::on_card()) context::offer_full_output(kPage, written);
    else storage::remove(kPage);
    return (title.isEmpty() ? String() : "《" + utf8_sanitize(title) + "》\n") + url + "\n" +
           utf8_head(utf8_sanitize(head), kPageHead);
}

}  // namespace vat::web
