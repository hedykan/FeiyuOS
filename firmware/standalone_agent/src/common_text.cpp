// The portable half of common.h: UTF-8, cJSON accessors, targets and shell
// quoting.  No ESP-IDF or storage calls here, so this file also builds on the
// host for tests/host; chip-specific helpers stay in common.cpp.
#include "common.h"

namespace vat {

String utf8_head(const String& text, std::size_t max_bytes) {
    if (text.length() <= max_bytes) return text;
    std::size_t length = max_bytes;
    while (length > 0 &&
           (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return text.substring(0, length);
}

String utf8_tail(const String& text, std::size_t max_bytes) {
    if (text.length() <= max_bytes) return text;
    std::size_t start = text.length() - max_bytes;
    while (start < text.length() &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        ++start;
    }
    return text.substring(start);
}

String utf8_sanitize(const String& text) {
    const std::size_t n = text.length();
    std::size_t i = 0;
    while (i < n) {  // fast path: return as is when already valid
        const uint8_t c = text[i];
        std::size_t len = c == 0 ? 0 : c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && i + len <= n && !(len == 2 && c < 0xC2);
        for (std::size_t k = 1; ok && k < len; ++k) ok = (static_cast<uint8_t>(text[i + k]) & 0xC0) == 0x80;
        if (!ok) break;
        i += len;
    }
    if (i == n) return text;
    String clean;
    clean.reserve(n);
    clean.concat(text.c_str(), i);
    while (i < n) {
        const uint8_t c = text[i];
        std::size_t len = c == 0 ? 0 : c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && i + len <= n && !(len == 2 && c < 0xC2);
        for (std::size_t k = 1; ok && k < len; ++k) ok = (static_cast<uint8_t>(text[i + k]) & 0xC0) == 0x80;
        if (ok) {
            clean.concat(text.c_str() + i, len);
            i += len;
        } else {
            clean += '?';
            ++i;
        }
    }
    return clean;
}

String json_str(const cJSON* object, const char* key, const String& fallback) {
    const cJSON* value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    if (cJSON_IsString(value) && value->valuestring) return String(value->valuestring);
    return fallback;
}

long json_int(const cJSON* object, const char* key, long fallback) {
    const cJSON* value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    if (cJSON_IsNumber(value)) return static_cast<long>(value->valuedouble);
    if (cJSON_IsString(value) && value->valuestring) return String(value->valuestring).toInt();
    return fallback;
}

bool json_bool(const cJSON* object, const char* key, bool fallback) {
    const cJSON* value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    if (cJSON_IsBool(value)) return cJSON_IsTrue(value);
    return fallback;
}

cJSON* json_obj(const cJSON* object, const char* key) {
    cJSON* value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    return cJSON_IsObject(value) ? value : nullptr;
}

cJSON* json_arr(const cJSON* object, const char* key) {
    cJSON* value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    return cJSON_IsArray(value) ? value : nullptr;
}

String json_print(const cJSON* value) {
    if (!value) return "null";
    char* printed = cJSON_PrintUnformatted(value);
    if (!printed) return "null";
    String result(printed);
    cJSON_free(printed);
    return result;
}

String target_path(const String& target) {
    const int mark = target.indexOf('?');
    String path = mark < 0 ? target : target.substring(0, mark);
    while (path.endsWith("/")) path.remove(path.length() - 1);
    return path;
}

String target_param(const String& target, const char* key, bool rest_of_query) {
    const int mark = target.indexOf('?');
    if (mark < 0) return {};
    const String query = target.substring(mark + 1);
    const String prefix = String(key) + "=";
    int start = 0;
    while (start <= static_cast<int>(query.length())) {
        int end = query.indexOf('&', start);
        if (end < 0 || rest_of_query) end = query.length();
        if (query.substring(start).startsWith(prefix)) return query.substring(start + prefix.length(), end);
        const int next = query.indexOf('&', start);
        if (next < 0) break;
        start = next + 1;
    }
    return {};
}

String url_decode(const String& text) {
    String out;
    for (std::size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '%' && i + 2 < text.length()) {
            out += static_cast<char>(strtol(text.substring(i + 1, i + 3).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

String shell_quote(const String& value) {
    if (value.isEmpty()) return "''";
    bool safe = true;
    for (std::size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
              c == '.' || c == '/' || c == ':' || c == '=' || c == '@' ||
              c == '%' || c == '+' || c == ',')) {
            safe = false;
            break;
        }
    }
    if (safe) return value;
    String quoted = "'";
    for (std::size_t i = 0; i < value.length(); ++i) {
        if (value[i] == '\'') {
            quoted += "'\\''";
        } else {
            quoted += value[i];
        }
    }
    quoted += "'";
    return quoted;
}

String shell_join(const std::vector<String>& argv) {
    String joined;
    for (const auto& value : argv) {
        if (!joined.isEmpty()) joined += ' ';
        joined += shell_quote(value);
    }
    return joined;
}

}  // namespace vat
