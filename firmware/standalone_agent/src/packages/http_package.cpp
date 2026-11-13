// http: plain HTTP(S) requests, for programs (vat.read / vat.write) and the agent.
//   read  https://host/path            GET (allowed: a read)
//   write https://host/path  content   POST, or options.method PUT | PATCH | DELETE (confirmed)
// Options (extra fields of the call): headers {名: 值}; save file:///vat/…
// (a 2xx body goes to that file instead of the reply); max_bytes (≤ 16 KB kept).
// A 2xx reply is the body itself; any other status is an ERROR with the start
// of the body, so a Lua program gets nil, err.  Programs need an @uses for
// every site, reads included (lua_vm).
//
// A header value may hold {secret:名字}: typed once on the device (never by the
// model), stored in /vat/secrets.json and sent only to the host it was typed
// for.  The value is wiped from replies and never logged.
#include "../common.h"
#include "../net.h"
#include "../registry.h"
#include "../settings_store.h"
#include "../storage.h"

namespace vat::registry {
namespace {

constexpr std::size_t kMaxBody = 16384;
constexpr std::size_t kDefaultBody = 8192;
constexpr char kUsage[] =
    "HTTP 规范：调网络接口用 read target=https://网址 发 GET；write target=https://网址 content=请求体 发 POST"
    "（另给 method=PUT、PATCH 或 DELETE），写需要用户确认。可加 headers={名: 值}；要密钥时在值里写 {secret:名字}"
    "（如 Authorization: Bearer {secret:github}），设备会弹出输入框让用户输入一次，不要向用户索要密钥。"
    "响应大时加 save=file:///vat/路径 存到卡上。搜索、读网页仍用 web://。";

bool is_http(const String& target) { return target.startsWith("http://") || target.startsWith("https://"); }

// The host of an http(s) URL, lower case; "" when the URL has none or carries user@.
String host_of(const String& url) {
    const int start = url.indexOf("://") + 3;
    int end = url.length();
    for (const char stop : {'/', '?', '#'}) {
        const int at = url.indexOf(stop, start);
        if (at >= 0 && at < end) end = at;
    }
    String host = url.substring(start, end);
    if (host.indexOf('@') >= 0) return {};
    const int colon = host.lastIndexOf(':');
    if (colon >= 0 && host.indexOf(']') < colon) host = host.substring(0, colon);
    host.toLowerCase();
    return host;
}

bool secret_name_ok(const String& name) {
    if (name.isEmpty() || name.length() > 32) return false;
    for (std::size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

struct Built {
    std::vector<std::pair<String, String>> headers;
    std::vector<String> secrets;  // the values put in, wiped from the reply
    String missing;               // a {secret:名字} with no value yet
    String error;
    ~Built() {
        for (auto& header : headers) wipe(header.second);
        for (auto& value : secrets) wipe(value);
    }
};

// The call's headers with {secret:名字} filled in for `host`.
void build_headers(const cJSON* arguments, const String& host, Built& out) {
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(arguments, "headers");
    cJSON* parsed = nullptr;
    if (cJSON_IsString(list)) list = parsed = cJSON_Parse(list->valuestring);  // the model may send JSON text
    if (list && !cJSON_IsObject(list)) {
        out.error = "headers 要是 {名: 值}";
        cJSON_Delete(parsed);
        return;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        const String name = item->string;
        String value = cJSON_IsString(item) ? String(item->valuestring) : json_print(item);
        if (name.isEmpty() || name.indexOf(':') >= 0 || name.indexOf('\n') >= 0 || name.indexOf('\r') >= 0 ||
            value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0) {
            out.error = "请求头「" + name + "」不合法（不能含冒号或换行）";
            break;
        }
        int at;
        while ((at = value.indexOf("{secret:")) >= 0) {
            const int close = value.indexOf('}', at);
            const String secret = close > at ? value.substring(at + 8, close) : String();
            if (!secret_name_ok(secret)) {
                out.error = "密钥名要是 1–32 个字母、数字或 _ - .，写成 {secret:名字}";
                break;
            }
            String stored, bound;
            if (!settings_store::http_secret(secret, stored, bound)) {
                if (out.missing.isEmpty()) out.missing = secret;
                value = value.substring(0, at) + value.substring(close + 1);
                continue;
            }
            if (bound != host) {
                wipe(stored);
                out.error = "密钥「" + secret + "」只能发给 " + bound + "，不能发给 " + host;
                break;
            }
            value = value.substring(0, at) + stored + value.substring(close + 1);
            out.secrets.push_back(stored);
            wipe(stored);
        }
        if (!out.error.isEmpty()) {
            wipe(value);
            break;
        }
        out.headers.emplace_back(name, value);
        wipe(value);
    }
    cJSON_Delete(parsed);
}

class HttpPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "https://网址"; }

    const Manifest& manifest() const override {
        static const Manifest m{"http", "HTTP 接口：GET / POST / PUT / PATCH / DELETE，自定义请求头，密钥只发给它的网站",
                                "http https 接口 api 请求 get post put delete webhook rest json 调用 网址", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && is_http(json_str(arguments, "target"));
    }

    String needs_secret(const String& tool, const cJSON* arguments) override {
        const String host = host_of(json_str(arguments, "target"));
        Built built;
        build_headers(arguments, host, built);
        if (built.missing.isEmpty() || !built.error.isEmpty()) return {};
        return "「" + built.missing + "」的密钥（只发给 " + host + "）";
    }

    String execute_secret(const String& tool, const cJSON* arguments, bool approved, const String& secret) override {
        const String host = host_of(json_str(arguments, "target"));
        Built built;
        build_headers(arguments, host, built);
        if (!built.missing.isEmpty() && built.error.isEmpty()) {
            const String saved = settings_store::set_http_secret(built.missing, host, secret);
            if (!saved.isEmpty()) return "ERROR: " + saved;
        }
        return execute(tool, arguments, approved);
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        net::Request request;
        request.url = json_str(arguments, "target");
        const String host = host_of(request.url);
        if (host.isEmpty()) return "ERROR: 网址不对（要 http(s)://主机/路径，不能带 用户@）";
        if (tool == "write") {
            request.method = json_str(arguments, "method", "POST");
            request.method.toUpperCase();
            if (request.method != "POST" && request.method != "PUT" && request.method != "PATCH" &&
                request.method != "DELETE")
                return "ERROR: method 只能是 POST、PUT、PATCH、DELETE（GET 用 read）";
            request.body = json_str(arguments, "content");
        }
        Built built;
        build_headers(arguments, host, built);
        if (!built.error.isEmpty()) return "ERROR: " + built.error;
        if (!built.missing.isEmpty()) return "ERROR: 还没有密钥「" + built.missing + "」，需要在设备上输入";
        request.headers = built.headers;
        const String save = json_str(arguments, "save");
        if (!save.isEmpty()) {
            request.save_path = storage::vat_path(save);
            const String why = storage::refused_path(request.save_path, true);
            if (!why.isEmpty()) return "ERROR: " + why;
        }
        const cJSON* max = cJSON_GetObjectItemCaseSensitive(arguments, "max_bytes");
        request.max_body = cJSON_IsNumber(max) && max->valuedouble >= 1
                               ? std::min<std::size_t>(kMaxBody, static_cast<std::size_t>(max->valuedouble))
                               : kDefaultBody;
        if (net::state() != net::WifiState::Connected) return "ERROR: Wi-Fi 未连接";

        net::HttpResult result = net::request(request);
        for (auto& header : request.headers) wipe(header.second);
        for (const auto& value : built.secrets) {
            if (value.length() >= 4) result.body.replace(value, "***");
        }
        const String status = String(result.status);
        if (result.status <= 0) {
            return result.status == -4 ? "ERROR: 响应没能存到 " + request.save_path
                                       : "ERROR: 请求失败：" + result.error;
        }
        if (result.error == "response too large") {
            return "ERROR: HTTP " + status + "，响应超过 " + String(request.max_body) +
                   " 字节，加 save=file:///vat/路径 存到卡上";
        }
        if (!result.ok()) return "ERROR: HTTP " + status + "：" + utf8_head(result.body, 300);
        if (!request.save_path.isEmpty()) {
            return "HTTP " + status + "，已保存 " + String(storage::size(request.save_path)) + " 字节到 " +
                   request.save_path;
        }
        if (result.body.isEmpty()) return "HTTP " + status + "（无内容）";
        return result.body;
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return {};
        String method = json_str(arguments, "method", "POST");
        method.toUpperCase();
        return method + " " + utf8_head(json_str(arguments, "target"), 120) + "（请求体 " +
               String(json_str(arguments, "content").length()) + " 字节）";
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"调接口", "", "查一下 arduino-esp32 在 GitHub 上的最新版本号"});
    }

    void resources(cJSON* list, int& remaining) override {
        add_resource(list, remaining, "https://", "http");
    }
};

}  // namespace

Package* make_http_package() { return new HttpPackage(); }

}  // namespace vat::registry
