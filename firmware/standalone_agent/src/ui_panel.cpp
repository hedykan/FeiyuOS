#include "ui_panel.h"

#include <cJSON.h>

#include <cstring>

namespace vat::ui_panel {
namespace {

bool text_list(const cJSON* root, const char* key, std::size_t max, std::vector<String>& out, String& error) {
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!list) return true;
    if (!cJSON_IsArray(list)) {
        error = String(key) + " 要是文字数组";
        return false;
    }
    if (static_cast<std::size_t>(cJSON_GetArraySize(list)) > max) {
        error = String(key) + " 最多 " + String(static_cast<int>(max)) + " 项";
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (!cJSON_IsString(item)) {
            error = String(key) + " 里只能是文字";
            return false;
        }
        out.push_back(utf8_head(String(item->valuestring), kMaxText));
    }
    return true;
}

// "30s", "5m", "1h", or a bare number of seconds; 0 when invalid.
uint32_t seconds(const String& text) {
    if (text.isEmpty()) return 0;
    const char unit = text[text.length() - 1];
    const String digits = (unit >= '0' && unit <= '9') ? text : text.substring(0, text.length() - 1);
    for (std::size_t i = 0; i < digits.length(); ++i) {
        if (digits[i] < '0' || digits[i] > '9') return 0;
    }
    const long n = digits.toInt();
    if (n <= 0) return 0;
    if (unit == 'm') return static_cast<uint32_t>(n * 60);
    if (unit == 'h') return static_cast<uint32_t>(n * 3600);
    return (unit == 's' || (unit >= '0' && unit <= '9')) ? static_cast<uint32_t>(n) : 0;
}

}  // namespace

bool parse(const String& json, Spec& out, String& error) {
    out = Spec();
    if (json.length() > kMaxJson) {
        error = "面板描述超过 " + String(static_cast<int>(kMaxJson)) + " 字节";
        return false;
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        error = "面板描述要是 JSON 对象";
        return false;
    }
    bool ok = true;
    const cJSON* field = nullptr;
    cJSON_ArrayForEach(field, root) {
        const char* k = field->string;
        if (std::strcmp(k, "title") && std::strcmp(k, "lines") && std::strcmp(k, "choices") && std::strcmp(k, "source")) {
            error = String("不认识的字段 ") + k + "（只能有 title、lines、choices、source）";
            ok = false;
            break;
        }
    }
    if (ok) {
        out.title = utf8_head(json_str(root, "title"), kMaxText);
        ok = text_list(root, "lines", kMaxLines, out.lines, error) &&
             text_list(root, "choices", kMaxChoices, out.choices, error);
    }
    const cJSON* source = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (ok && source) {
        const String read = json_str(source, "read"), exec = json_str(source, "exec"), tool = json_str(source, "tool");
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(source, "arguments");
        const int forms = !read.isEmpty() + !exec.isEmpty() + !tool.isEmpty();
        if (!cJSON_IsObject(source) || forms != 1 || (!tool.isEmpty() && !cJSON_IsObject(args))) {
            error = "source 写 read 或 exec 加目标，或 tool 加 arguments，再加 every，如 source 为 read sensor://gps/location、every 30s";
            ok = false;
        } else {
            out.source_tool = tool.isEmpty() ? (read.isEmpty() ? "exec" : "read") : tool;
            if (tool.isEmpty()) {
                cJSON* bare = cJSON_CreateObject();
                cJSON_AddStringToObject(bare, "target", (read.isEmpty() ? exec : read).c_str());
                out.source_args = json_print(bare);
                cJSON_Delete(bare);
            } else {
                out.source_args = json_print(args);
            }
            const String every = json_str(source, "every");
            out.every_s = every.isEmpty() ? 0 : seconds(every);
            if (!every.isEmpty() && out.every_s < kMinEverySeconds) {
                error = "source.every 至少 " + String(static_cast<int>(kMinEverySeconds)) + " 秒，如 30s、5m";
                ok = false;
            }
        }
    }
    if (ok && out.title.isEmpty() && out.lines.empty() && out.choices.empty() && out.source_tool.isEmpty()) {
        error = "面板是空的：至少要有 title、lines、choices 或 source";
        ok = false;
    }
    cJSON_Delete(root);
    return ok;
}

String to_json(const Spec& spec) {
    cJSON* root = cJSON_CreateObject();
    if (!spec.title.isEmpty()) cJSON_AddStringToObject(root, "title", spec.title.c_str());
    const auto add_list = [root](const char* key, const std::vector<String>& items) {
        if (items.empty()) return;
        cJSON* list = cJSON_AddArrayToObject(root, key);
        for (const auto& item : items) cJSON_AddItemToArray(list, cJSON_CreateString(item.c_str()));
    };
    add_list("lines", spec.lines);
    add_list("choices", spec.choices);
    if (!spec.source_tool.isEmpty()) {
        cJSON* source = cJSON_AddObjectToObject(root, "source");
        cJSON_AddStringToObject(source, "tool", spec.source_tool.c_str());
        cJSON* args = cJSON_Parse(spec.source_args.c_str());
        cJSON_AddItemToObject(source, "arguments", args ? args : cJSON_CreateObject());
        if (spec.every_s) cJSON_AddStringToObject(source, "every", (String(static_cast<unsigned>(spec.every_s)) + "s").c_str());
    }
    const String out = json_print(root);
    cJSON_Delete(root);
    return out;
}

std::vector<String> lines_from(const String& text) {
    std::vector<String> out;
    std::size_t start = 0;
    while (start <= text.length() && out.size() < kMaxLines) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = static_cast<int>(text.length());
        String line = text.substring(start, end);
        line.trim();
        if (!line.isEmpty()) out.push_back(utf8_head(line, kMaxText));
        start = end + 1;
    }
    return out;
}

bool valid_name(const String& name) {
    if (name.isEmpty() || name.length() > 48) return false;
    for (const char c : {'/', '?', '#', '&', '{', '}', '"', '\'', '\\'}) {
        if (name.indexOf(c) >= 0) return false;
    }
    return true;
}

}  // namespace vat::ui_panel
