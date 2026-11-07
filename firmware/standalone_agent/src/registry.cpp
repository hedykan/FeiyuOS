#include "registry.h"

#include <algorithm>

#include "common.h"
#include "ops_ssh.h"

namespace vat::registry {
namespace {

std::vector<Package*> installed;

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
// targets — "设备" is a local keyword — and no lora://).
String discover_resources(const String& query, int limit) {
    cJSON* list = cJSON_CreateArray();
    int remaining = limit;
    const auto query_terms = terms(query);
    std::vector<std::pair<int, Package*>> ranked;
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
        text += String(++count) + ". " + manifest.id + "：" + manifest.summary;
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
const std::vector<const char*>& usages() {
    static std::vector<const char*> out;  // worker task only; rebuilt per request
    out.clear();
    for (auto* package : installed) {
        const char* text = package->usage();
        if (!text || !*text) continue;
        bool safe = true;
        for (const char* c = text; *c && safe; ++c) safe = *c != '"' && *c != '\\' && static_cast<unsigned char>(*c) >= 0x20;
        if (safe) out.push_back(text);
        else runtime_event("package_usage_unsafe", package->manifest().id);
    }
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
    for (auto* package : installed) package->loop();
}

String execute(const String& tool, const cJSON* arguments, bool approved, const String& secret) {
    for (auto* package : installed) {
        if (!package->supports(tool, arguments)) continue;
        return secret.isEmpty() ? package->execute(tool, arguments, approved)
                                : package->execute_secret(tool, arguments, approved, secret);
    }
    // No package owns it: say so instead of guessing.
    String what = json_str(arguments, "target");
    if (what.isEmpty()) what = json_str(arguments, "action");
    return "没有能力包可处理 " + tool + "：" + (what.isEmpty() ? String("未指定") : what);
}

}  // namespace vat::registry
