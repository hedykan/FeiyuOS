#include <new>
#include "address_map.h"
#include "registry.h"

#include <algorithm>
#include <mutex>

#include "common.h"
#include "ops_ssh.h"

namespace vat::registry {
namespace {

std::vector<Package*> installed;

// Open streams: requested by any task, opened and sampled on the UI task.
struct OpenStream {
    String target;
    Package* owner = nullptr;
    Package::StreamSpec spec{};
    Package::StreamValue value;
    bool opened = false;
    bool have = false;
    String error;
    unsigned long last_sample = 0;
    unsigned long last_read = 0;
};
std::vector<OpenStream> open_streams;
std::mutex streams_lock;
TaskHandle_t ui_task = nullptr;  // the task that runs loop(): it samples
constexpr unsigned long kStreamIdleMs = 10000;

String stream_base(const String& target) {
    const int q = target.indexOf('?');
    return q < 0 ? target : target.substring(0, q);
}

// A declared target ending in '*' stands for every target with that prefix
// (device://local/ble/rssi/* covers device://local/ble/rssi/aa:bb:…); otherwise the target itself.
bool stream_matches(const String& base, const char* declared) {
    const size_t n = strlen(declared);
    if (n > 0 && declared[n - 1] == '*') return base.length() >= n && strncmp(base.c_str(), declared, n - 1) == 0;
    return base == declared;
}

bool find_stream(const String& target, Package::StreamSpec& spec, Package** owner) {
    const String base = stream_base(target);
    for (auto* package : installed) {
        std::vector<Package::StreamSpec> specs;
        package->streams(specs);
        for (const auto& s : specs) {
            if (stream_matches(base, s.target)) {
                spec = s;
                if (owner) *owner = package;
                return true;
            }
        }
    }
    return false;
}

String number_text(double v) {
    char text[32];
    snprintf(text, sizeof(text), "%.6f", v);
    String s(text);
    while (s.endsWith("0")) s.remove(s.length() - 1);
    if (s.endsWith(".")) s.remove(s.length() - 1);
    return s == "-0" ? String("0") : s;
}

String value_json(const Package::StreamSpec& spec, const Package::StreamValue& value) {
    String out = "{";
    String fields = spec.fields;
    int i = 0;
    while (fields.length() > 0 && i < value.count) {
        fields.trim();
        const int space = fields.indexOf(' ');
        const String name = space < 0 ? fields : fields.substring(0, space);
        fields = space < 0 ? String() : fields.substring(space + 1);
        if (name.isEmpty()) continue;
        out += String(i ? "," : "") + "\"" + name + "\":" + number_text(value.v[i]);
        ++i;
    }
    return out + "}";
}

void sample_streams() {
    std::lock_guard<std::mutex> guard(streams_lock);
    const unsigned long now = millis();
    for (std::size_t i = 0; i < open_streams.size();) {
        OpenStream& s = open_streams[i];
        if (s.opened && now - s.last_read > kStreamIdleMs) {  // nobody reads it any more
            s.owner->close_stream(s.target);
            open_streams.erase(open_streams.begin() + i);
            continue;
        }
        if (!s.opened && s.error.isEmpty()) {
            s.error = s.owner->open_stream(s.target);
            s.opened = s.error.isEmpty();
        }
        if (s.opened && (!s.have || now - s.last_sample >= s.spec.period_ms)) {
            Package::StreamValue v;
            if (s.owner->sample(s.target, v)) {
                s.value = v;
                s.have = true;
            }
            s.last_sample = now;
        }
        ++i;
    }
}

bool is_cjk_token(const String& token, std::vector<String>& chars) {
    chars.clear();
    for (std::size_t i = 0; i < token.length();) {
        const unsigned char c = token[i];
        if ((c & 0xF0) != 0xE0 || i + 2 >= token.length()) return false;
        const uint32_t cp = ((c & 0x0F) << 12) | ((token[i + 1] & 0x3F) << 6) | (token[i + 2] & 0x3F);
        if (cp < 0x3400 || cp > 0x9FFF) return false;
        chars.push_back(token.substring(i, i + 3));
        i += 3;
    }
    return !chars.empty();
}

// Split on spaces and punctuation; pure-CJK tokens longer than two characters
// are also expanded into bigrams so "容器日志" matches "日志".
std::vector<String> terms(const String& text) {
    String lowered = text;
    lowered.toLowerCase();
    for (const char* sep : {"，", "。", "：", "、"}) lowered.replace(sep, " ");
    for (const char sep : {',', ':', '/', '\t', '\n'}) lowered.replace(String(sep), " ");
    std::vector<String> result;
    int start = 0;
    while (start < static_cast<int>(lowered.length())) {
        int end = lowered.indexOf(' ', start);
        if (end < 0) end = lowered.length();
        const String token = lowered.substring(start, end);
        start = end + 1;
        if (token.isEmpty()) continue;
        result.push_back(token);
        std::vector<String> chars;
        if (is_cjk_token(token, chars) && chars.size() > 2) {
            for (std::size_t i = 0; i + 1 < chars.size(); ++i) result.push_back(chars[i] + chars[i + 1]);
        }
    }
    return result;
}

String to_json(cJSON* root) {
    const String json = json_print(root);
    cJSON_Delete(root);
    return json;
}

// Lists are returned as short numbered text ("1. …"): they stay within the
// tool-result size kept in memory, and "the 2nd one" can be resolved later.
// Text a query is matched against: id, summary, keywords, commands.
String manifest_text(const Package& package) {
    const auto& manifest = package.manifest();
    String haystack = String(manifest.id) + " " + manifest.summary + " " + manifest.keywords;
    for (const char* command : manifest.commands) haystack += String(" ") + command;
    haystack.toLowerCase();
    return haystack;
}

// 10 per term naming the package itself, 1 per term found in its manifest.
int match_score(const Package& package, const std::vector<String>& query_terms) {
    String id = package.manifest().id;
    id.toLowerCase();
    const String haystack = manifest_text(package);
    int score = 0;
    for (const auto& term : query_terms) {
        if (term == id) score += 10;
        else if (haystack.indexOf(term) >= 0) score += 1;
    }
    return score;
}

bool matches(const Package& package, const std::vector<String>& query_terms) {
    return match_score(package, query_terms) > 0;
}

// Packages list their resources best match first, so a package registered
// late is not crowded out by the limit ("LoRa 设备" once returned eight local
// targets — "设备" is a local keyword — and no device://local/lora/).
String discover_resources(const String& query, int limit) {
    cJSON* list = cJSON_CreateArray();
    int remaining = limit;
    const auto query_terms = terms(query);
    std::vector<std::pair<int, Package*>> ranked;
    for (auto* package : installed) package->find(query_terms, list, remaining);  // what the query names, first
    for (auto* package : installed) ranked.push_back({match_score(*package, query_terms), package});
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& entry : ranked) {
        if (remaining <= 0) break;
        entry.second->resources(list, remaining);
        std::vector<Package::TriggerSpec> events;
        entry.second->triggers(events);
        for (const auto& spec : events) {
            if (!add_resource(list, remaining, spec.target, (String("事件：") + spec.description).c_str())) break;
        }
        std::vector<Package::StreamSpec> specs;
        entry.second->streams(specs);
        for (const auto& spec : specs) {
            const String kind = String("流：") + spec.description + "；字段 " + spec.fields +
                                (spec.unit[0] ? String("（") + spec.unit + "）" : String()) + "，约每 " +
                                String(spec.period_ms) + "ms 一个新值";
            if (!add_resource(list, remaining, spec.target, kind.c_str())) break;
        }
    }
    String text;
    int n = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        text += String(++n) + ". " + json_str(item, "target");
        const cJSON* connected = cJSON_GetObjectItemCaseSensitive(item, "connected");
        if (cJSON_IsBool(connected)) text += cJSON_IsTrue(connected) ? "（已连接）" : "（未连接）";
        const String kind = json_str(item, "kind");
        if (kind.startsWith("事件：")) text += "（" + kind + "，可作触发）";
        if (kind.startsWith("流：")) text += "（" + kind + "；程序里用 vat.stream(它) 取最新值）";
        text += "\n";
    }
    cJSON_Delete(list);
    return text.isEmpty() ? String("没有可见资源") : text;
}

String discover_capabilities(const String& query, const String& target, int limit, bool filter = true) {
    const auto query_terms = terms(query);
    const String profile = profile_of(target);
    String text;
    int count = 0;
    for (auto* package : installed) {
        const auto& manifest = package->manifest();
        if (filter && !query.isEmpty() && !matches(*package, query_terms)) continue;
        if (count >= limit) break;
        text += String(++count) + ". " + manifest.id + "：" + manifest.summary + (package->present() ? "" : "（未接上，现在用不了）");
        if (!profile.isEmpty() && manifest.availability_program) {
            text += package->available(profile) ? "（" + profile + " 上可用）" : "（" + profile + " 上不可用）";
        }
        text += "\n";
    }
    return text;
}

// Every package by name only: a few bytes each, so the list is never cut
// however many packages there are.  Details come from a query by name.
String package_names() {
    String names;
    int count = 0;
    for (auto* package : installed) names += String(count++ ? "、" : "") + package->manifest().id;
    return "共 " + String(count) + " 个能力包：" + names + "\n（看某个包的用途和用法：discover query 写包名或关键词）";
}

String discover_commands(const String& query, int limit) {
    const auto query_terms = terms(query);
    struct Ranked {
        int score;
        String command;
        const Package* package;
    };
    std::vector<Ranked> ranked;
    for (auto* package : installed) {
        const auto& manifest = package->manifest();
        for (const char* command : manifest.commands) {
            String haystack = String(command) + " " + manifest.keywords + " " + package->search_text(command);
            haystack.toLowerCase();
            int score = 0;
            for (const auto& term : query_terms) score += haystack.indexOf(term) >= 0 ? 1 : 0;
            if (query.isEmpty() || score) ranked.push_back({score, command, package});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.score != b.score ? a.score > b.score : a.command < b.command;
    });
    String text;
    for (int i = 0; i < static_cast<int>(ranked.size()) && i < limit; ++i) {
        const bool read_only = String(ranked[i].package->command_risk(ranked[i].command)) == "read_only";
        text += String(i + 1) + ". " + ranked[i].command + "（" + (read_only ? "只读" : "需确认") + "）\n";
    }
    return text.isEmpty() ? String("没有匹配的命令") : text;
}

String discover_help(const String& query, const String& target) {
    String trimmed = query;
    trimmed.trim();
    if (trimmed.isEmpty()) return "{\"error\":\"help 查询需要 program 或 program subcommand\"}";
    const int space = trimmed.indexOf(' ');
    const String program = space >= 0 ? trimmed.substring(0, space) : trimmed;
    String subcommand = space >= 0 ? trimmed.substring(space + 1) : String();
    subcommand.trim();
    const int extra = subcommand.indexOf(' ');
    if (extra >= 0) subcommand = subcommand.substring(0, extra);
    bool known = false;
    for (auto* package : installed) {
        for (const char* command : package->manifest().commands) {
            const String c(command);
            known = known || c == program || c.startsWith(program + " ");
        }
    }
    if (!known) return "{\"error\":\"该命令不在已注册能力包中\"}";
    const String profile = profile_of(target);
    if (profile.isEmpty()) return "{\"error\":\"当前 help 查询需要明确 ssh:// target\"}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "program", program.c_str());
    cJSON_AddStringToObject(root, "help", ssh::command_help(profile, program, subcommand).c_str());
    return to_json(root);
}

}  // namespace

bool open_lua(lua_State* L, const String& id) {
    for (auto* package : installed) {
        if (id == package->manifest().id) return package->open_lua(L);
    }
    return false;
}

bool add(Package* package) {
    for (auto* existing : installed) {
        if (String(existing->manifest().id) == package->manifest().id) {
            runtime_event("package_duplicate", package->manifest().id);
            return false;
        }
    }
    installed.push_back(package);
    return true;
}

const std::vector<Package*>& packages() { return installed; }

bool add_resource(cJSON* list, int& remaining, const char* target, const char* kind) {
    if (remaining <= 0) return false;
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "target", target);
    cJSON_AddStringToObject(item, "kind", kind);
    cJSON_AddItemToArray(list, item);
    --remaining;
    return true;
}
namespace {
bool json_safe(const char* text) {
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20) return false;
    }
    return true;
}

// Every system prompt carries this head and one line per on-demand package.
constexpr char kIndexHead[] =
    "能力包（要用某个包而不清楚它的地址或参数时，先 discover scope=capabilities query=包名 取它的用法；已经清楚的直接调用）：";
}  // namespace

const std::vector<const char*>& usages() {
    static std::vector<const char*> out;  // worker task only; rebuilt per request
    static String index;                  // the on-demand packages, one line each
    out.clear();
    index = kIndexHead;
    for (auto* package : installed) {
        const char* text = package->usage();
        if (!text || !*text || !package->present()) continue;
        const char* line = package->index();
        const auto& manifest = package->manifest();
        if (!json_safe(text) || (line && (!json_safe(line) || !json_safe(manifest.summary)))) {
            runtime_event("package_usage_unsafe", manifest.id);
            continue;
        }
        if (!line) {
            out.push_back(text);
            continue;
        }
        index += String("\\n") + manifest.id + "：" + manifest.summary + "（" + line + "）";
    }
    index += "\\n";
    out.push_back(index.c_str());
    return out;
}

String profile_of(const String& target) {
    String profile, path, error;
    return ssh::parse_target(target, profile, path, error) ? profile : String();
}

String discover(const cJSON* arguments) {
    const String scope = json_str(arguments, "scope", "capabilities");
    String query = json_str(arguments, "query");
    query.trim();
    const String target = json_str(arguments, "target");
    const int limit = constrain(json_int(arguments, "limit", 8), 1, 32);  // more than 20 packages since v0.6
    if (scope == "resources") return discover_resources(query, limit);
    if (scope == "capabilities") {
        // A query that matches nothing (e.g. "all capabilities") returns the
        // full list instead of an empty one, flagged as unmatched.
        // No query: all packages, names only (never cut).  A query lists the
        // matching ones with what they do, up to `limit`.
        if (query.isEmpty()) return package_names();
        // A query naming on-demand packages (their usage is not in the system
        // prompt): their summary and usage, at most two.
        String named;
        int count = 0;
        const auto words = terms(query);
        for (auto* package : installed) {
            if (!package->index() || count == 2) continue;
            const String id = package->manifest().id;
            if (std::find(words.begin(), words.end(), id) == words.end()) continue;
            named += String(count++ ? "\n\n" : "") + id + "：" +
                     (package->present() ? package_help(id) : String(package->manifest().summary) + "（未接上，现在用不了）");
        }
        if (!named.isEmpty()) return named;
        const String matched = discover_capabilities(query, target, limit);
        if (!matched.isEmpty()) return matched;
        return "没有精确匹配。" + package_names();
    }
    if (scope == "commands") return discover_commands(query, limit);
    if (scope == "help") return discover_help(query, target);
    return "{\"error\":\"未知 discover scope\"}";
}

String needs_secret(const String& tool, const cJSON* arguments) {
    for (auto* package : installed) {
        if (package->supports(tool, arguments)) return package->needs_secret(tool, arguments);
    }
    return {};
}

bool reversible(const String& tool, const cJSON* arguments) {
    for (auto* package : installed) {
        if (package->supports(tool, arguments)) return package->reversible(tool, arguments);
    }
    return false;
}

std::vector<Package::Hint> hints() {
    std::vector<Package::Hint> out;
    for (auto* package : installed) {
        const std::size_t from = out.size();
        package->hints(out);
        for (std::size_t i = from; i < out.size(); ++i) out[i].package = package->manifest().id;
    }
    return out;
}

String package_help(const String& id) {
    for (auto* package : installed) {
        if (id != package->manifest().id) continue;
        const char* usage = package->usage();
        return String(package->manifest().summary) + (usage ? String("\n\n") + usage : String());
    }
    return {};
}

String watch(const String& target, String& normalized) {
    normalized = target;
    for (auto* package : installed) {
        String error;
        if (package->watch(target, normalized, error)) return error;
    }
    return "没有扩展包能发出这个事件：" + target + "（用 discover 查 resources 里标“可作触发”的项）";
}

void unwatch(const String& target) {
    for (auto* package : installed) package->unwatch(target);
}

String confirm_summary(const String& tool, const cJSON* arguments) {
    for (auto* package : installed) {
        if (package->supports(tool, arguments)) return package->confirm_summary(tool, arguments);
    }
    return {};
}

void loop() {
    ui_task = xTaskGetCurrentTaskHandle();
    for (auto* package : installed) package->loop();
    sample_streams();
}

bool stream_spec(const String& target, Package::StreamSpec& spec) { return find_stream(target, spec, nullptr); }

String stream_names() {
    String names;
    for (auto* package : installed) {
        std::vector<Package::StreamSpec> specs;
        package->streams(specs);
        for (const auto& s : specs) names += String(names.isEmpty() ? "" : "、") + s.target;
    }
    return names.isEmpty() ? String("（没有）") : names;
}

bool read_stream(const String& target, String& out, uint32_t wait_ms) {
    Package::StreamSpec spec{};
    Package* owner = nullptr;
    if (!find_stream(target, spec, &owner)) {
        out = "ERROR: " + stream_base(target) + " 不是流；可订阅的有：" + stream_names();
        return false;
    }
    const String base = stream_base(target);
    const unsigned long deadline = millis() + wait_ms;
    while (true) {
        {
            std::lock_guard<std::mutex> guard(streams_lock);
            OpenStream* found = nullptr;
            for (auto& s : open_streams) {
                if (s.target == base) found = &s;
            }
            if (!found) {
                OpenStream s;
                s.target = base;
                s.owner = owner;
                s.spec = spec;
                open_streams.push_back(s);
                found = &open_streams.back();
            }
            found->last_read = millis();
            if (!found->error.isEmpty()) {
                out = "ERROR: " + base + " 打不开：" + found->error;
                open_streams.erase(open_streams.begin() + (found - open_streams.data()));  // try again next time
                return false;
            }
            if (found->have) {
                out = value_json(spec, found->value);
                return true;
            }
        }
        if (static_cast<long>(millis() - deadline) >= 0) {
            out = "ERROR: " + base + " 暂时没有数据（如 GPS 还没定位）";
            return false;
        }
        if (xTaskGetCurrentTaskHandle() == ui_task) sample_streams();  // on the UI task: nobody else will
        else delay(20);                                                 // the UI task opens and samples it
    }
}

bool failed(const String& result) {
    static const char* const kFailures[] = {"ERROR", "没有能力包可处理", "已拒绝", "SSH 连接失败", "SSH 失败",
                                            "命令不在自动只读允许范围"};
    for (const char* prefix : kFailures) {
        if (result.startsWith(prefix)) return true;
    }
    return false;
}

namespace {
cJSON* offered = nullptr;
String offered_text;
int wanted = 0;
}  // namespace

bool data_wanted() { return wanted > 0; }

void want_data(bool on) { wanted += on ? 1 : -1; }

void offer_data(cJSON* data, const String& text) {
    cJSON_Delete(offered);
    if (wanted <= 0) {  // nobody will take it
        cJSON_Delete(data);
        offered = nullptr;
        release(offered_text);
        return;
    }
    offered = data;
    offered_text = text;
}

cJSON* take_data(String* text) {
    cJSON* data = offered;
    offered = nullptr;
    if (text) *text = std::move(offered_text);
    release(offered_text);
    return data;
}

String execute(const String& tool, const cJSON* arguments, bool approved, const String& secret) {
    cJSON_Delete(take_data());  // an offer nobody took belongs to an earlier call
    const String target = json_str(arguments, "target");
    const String now = address::retired(target);
    if (!now.isEmpty()) return "ERROR: 地址已改为 " + now + "（" + target + " 是旧写法，不再使用）";
    for (auto* package : installed) {
        if (!package->supports(tool, arguments)) continue;
        // A package that runs out of memory (a container that cannot grow) must not
        // take the device down: the call fails and says so.
        try {
            return secret.isEmpty() ? package->execute(tool, arguments, approved)
                                    : package->execute_secret(tool, arguments, approved, secret);
        } catch (const std::bad_alloc&) {
            runtime_event("low_memory", tool + " " + utf8_head(json_str(arguments, "target"), 60));
            return "ERROR: 设备内存不足，这一步没有做成；换个更小的请求，或稍后再试";
        }
    }
    // No package owns it: say so instead of guessing.
    String what = json_str(arguments, "target");
    if (what.isEmpty()) what = json_str(arguments, "action");
    return "没有能力包可处理 " + tool + "：" + (what.isEmpty() ? String("未指定") : what);
}

}  // namespace vat::registry
