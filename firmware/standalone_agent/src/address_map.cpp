#include "address_map.h"

#include <cctype>
#include <cstring>

namespace vat::address {
namespace {

// A name in an address ends at white space, a quote, a closing bracket, |, >
// or Chinese punctuation (，。、；：）).
bool name_ends(const String& s, unsigned i) {
    if (i >= s.length()) return true;
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == ')' || c == '`' || c == '|' ||
        c == '>') {
        return true;
    }
    if (i + 2 >= s.length()) return false;
    static const char* const kPunctuation[] = {"，", "。", "、", "；", "：", "）"};
    for (const char* p : kPunctuation) {
        if (std::strncmp(s.c_str() + i, p, 3) == 0) return true;
    }
    return false;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

enum class Match : uint8_t { Prefix, Whole };  // Whole: not followed by / or a word character

struct Rule {
    const char* old_text;
    const char* new_text;
    Match match;
    bool not_after_word;  // ir:// inside another word or path is not ir://
};

const Rule kRules[] = {
    {"lora://settings/", "settings://lora/", Match::Prefix, false},
    {"lora://", "device://local/lora/", Match::Prefix, false},
    {"ble://power", "settings://ble/power", Match::Prefix, false},
    {"ble://devices", "device://local/ble/devices/list", Match::Whole, false},
    {"ble://scan/log", "device://local/ble/log", Match::Prefix, false},
    {"ble://", "device://local/ble/", Match::Prefix, false},
    {"wifi://", "device://local/wifi/", Match::Prefix, false},
    {"ir://", "device://local/ir/", Match::Prefix, true},
    {"device://local/display/brightness", "settings://brightness", Match::Prefix, false},
    {"device://local/display/timeout", "settings://timeout", Match::Prefix, false},
    {"device://local/audio/volume", "settings://volume", Match::Prefix, false},
    {"device://local/context/length", "settings://context", Match::Prefix, false},
    {"device://local/settings", "settings://", Match::Whole, false},
    {"device://boot", "device://local/event/boot", Match::Whole, false},
    {"ui://apps", "ui://apps/list", Match::Whole, false},
    {"memory://cases", "memory://cases/list", Match::Whole, false},
    {"memory://out", "memory://out/list", Match::Whole, false},
    {"pkg://install?", "pkg://new?", Match::Prefix, false},
    {"ui://canvas/", "gfx://canvas/", Match::Prefix, false},
};

String apply(const String& text, const Rule& rule) {
    const unsigned n = std::strlen(rule.old_text);
    String out;
    unsigned from = 0;
    while (true) {
        const int at = text.indexOf(rule.old_text, from);
        if (at < 0) break;
        const unsigned end = at + n;
        bool ok = true;
        if (rule.match == Match::Whole && end < text.length() && (text[end] == '/' || word_char(text[end]))) ok = false;
        if (rule.not_after_word && at > 0 && (word_char(text[at - 1]) || text[at - 1] == '/')) ok = false;
        out += text.substring(from, ok ? at : end);
        if (ok) out += rule.new_text;
        from = end;
    }
    return out + text.substring(from);
}

// "pkg://install/天气" -> "pkg://天气/install": the action moves to the end.
String action_last(const String& text, const char* scheme, const char* const* actions, std::size_t count) {
    String out;
    unsigned from = 0;
    const String head = String(scheme) + "://";
    while (true) {
        int best = -1;
        const char* found = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            const int at = text.indexOf(head + actions[i] + "/", from);
            if (at >= 0 && (best < 0 || at < best)) {
                best = at;
                found = actions[i];
            }
        }
        if (best < 0) break;
        const unsigned name_from = best + head.length() + std::strlen(found) + 1;
        unsigned name_to = name_from;
        while (!name_ends(text, name_to)) ++name_to;
        if (name_to == name_from) {  // no name: leave it
            out += text.substring(from, name_from);
            from = name_from;
            continue;
        }
        out += text.substring(from, best) + head + text.substring(name_from, name_to) + "/" + found;
        from = name_to;
    }
    return out + text.substring(from);
}

}  // namespace

String rewrite(const String& text) {
    String out = text;
    for (const auto& rule : kRules) out = apply(out, rule);
    static const char* const kPkgActions[] = {"install", "update", "rollback"};
    out = action_last(out, "pkg", kPkgActions, 3);
    static const char* const kOpen[] = {"open"};
    out = action_last(out, "settings", kOpen, 1);
    return out;
}

String retired(const String& target) {
    const String now = rewrite(target);
    return now == target ? String() : now;
}

}  // namespace vat::address
