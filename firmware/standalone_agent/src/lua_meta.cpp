#include "lua_meta.h"

namespace vat::lua_meta {
namespace {

const char* const kTools[] = {"read", "write", "exec", "observe", "input", "session"};

bool known_tool(const String& tool) {
    for (const char* t : kTools) {
        if (tool == t) return true;
    }
    return false;
}

// "a b c" -> "a" and "b c" (both trimmed).
void split_first(const String& text, String& first, String& rest) {
    int at = -1;
    for (std::size_t i = 0; i < text.length(); ++i) {
        if (text[i] == ' ' || text[i] == '\t') {
            at = static_cast<int>(i);
            break;
        }
    }
    first = at < 0 ? text : text.substring(0, at);
    rest = at < 0 ? String() : text.substring(at + 1);
    first.trim();
    rest.trim();
}

// A trailing "  -- comment" on a declaration is dropped.
String without_comment(String value) {
    const int at = value.indexOf(" --");
    if (at >= 0) value = value.substring(0, at);
    value.trim();
    return value;
}

String check_target(const String& target) {
    if (target.indexOf("://") <= 0) return "目标要写完整，如 ssh://开发机 或 https://example.com/*";
    const int star = target.indexOf('*');
    if (star >= 0 && star != static_cast<int>(target.length()) - 1) return "* 只能放在目标末尾";
    return {};
}

uint32_t fnv(uint32_t h, const String& text) {
    for (std::size_t i = 0; i < text.length(); ++i) {
        h ^= static_cast<uint8_t>(text[i]);
        h *= 16777619u;
    }
    return h;
}

}  // namespace

String parse(const String& source, Header& out) {
    out = Header();
    if (source.length() > kMaxSource) return "程序超过 " + String(static_cast<int>(kMaxSource / 1024)) + " KB";
    std::size_t at = 0;
    int line_no = 0;
    while (at < source.length()) {
        int end = source.indexOf('\n', at);
        if (end < 0) end = source.length();
        String line = source.substring(at, end);
        at = end + 1;
        ++line_no;
        line.trim();
        if (line.isEmpty()) continue;
        if (!line.startsWith("--")) break;  // the code starts: the header is over
        String body = line.substring(2);
        body.trim();
        if (!body.startsWith("@")) continue;  // an ordinary comment
        String key, value;
        split_first(body.substring(1), key, value);
        const String where = "第 " + String(line_no) + " 行 @" + key + "：";
        if (key == "uses") {
            String tool, target;
            split_first(without_comment(value), tool, target);
            if (!known_tool(tool)) return where + "要写成 @uses 动作 目标，动作是 read、write、exec、observe、input 或 session";
            const String problem = check_target(target);
            if (!problem.isEmpty()) return where + problem;
            out.uses.push_back({tool, target});
        } else if (key == "input") {
            String name, fallback;
            split_first(without_comment(value), name, fallback);
            if (name.isEmpty()) return where + "要写成 @input 参数名 [默认值]";
            for (const auto& i : out.inputs) {
                if (i.name == name) return where + "参数「" + name + "」重复了";
            }
            out.inputs.push_back({name, fallback, fallback.isEmpty()});
        } else if (key == "about") {
            out.about = value;
        } else if (key == "version") {
            out.version = without_comment(value);
        } else if (key == "package") {
            out.package = without_comment(value);
        } else if (key == "target") {
            out.target = without_comment(value);
            if (out.target.indexOf("://") <= 0) return where + "要写成 scheme://，如 weather://";
        } else if (key == "usage") {
            out.usage = value;
        } else if (key == "keywords") {
            out.keywords = value;
        } else {
            return where + "不认识（可用 @about @input @uses @version @package @target @usage @keywords）";
        }
    }
    if (!out.package.isEmpty() && out.target.isEmpty()) return "@package 要配一个 @target（这个包负责的 scheme://）";
    return {};
}

bool needs_confirm(const Header& header) { return !header.uses.empty(); }

bool covers(const std::vector<Use>& uses, const String& tool, const String& target) {
    for (const auto& u : uses) {
        if (u.tool != tool) continue;
        if (u.target.endsWith("*")) {
            if (target.startsWith(u.target.substring(0, u.target.length() - 1))) return true;
        } else if (target == u.target || target.startsWith(u.target + "/") || target.startsWith(u.target + "?")) {
            return true;
        }
    }
    return false;
}

uint32_t digest(const Header& header) {
    uint32_t h = 2166136261u;
    for (const auto& u : header.uses) h = fnv(fnv(h, u.tool + " "), u.target + "\n");
    h = fnv(fnv(h, header.package + "\n"), header.target);
    // A package's usage goes into the agent's system prompt: changing it
    // needs a new confirmation.  (Programs: unchanged, so their confirmations hold.)
    if (!header.package.isEmpty()) h = fnv(fnv(h, "\n"), header.usage);
    return h;
}

String describe(const Use& use) {
    static const char* const kWords[][2] = {{"read", "读取"},    {"write", "写入"}, {"exec", "执行"},
                                           {"observe", "查看"}, {"input", "输入"}, {"session", "会话"}};
    for (const auto& w : kWords) {
        if (use.tool == w[0]) return String(w[1]) + " " + use.target;
    }
    return use.tool + " " + use.target;
}

String check_name(const String& name) {
    if (name.isEmpty()) return "名字不能为空";
    if (name.length() > kMaxName) return "名字最长 " + String(static_cast<int>(kMaxName)) + " 字节（约 16 个汉字）";
    if (name[0] == '.' || name[0] == '~') return "名字不能以 . 或 ~ 开头";
    for (const char* verb : {"scan", "list", "catalog", "install", "update", "rollback", "old", "index", "approved", "triggers"}) {
        if (name == verb) return String("「") + verb + "」是保留字，换个名字";
    }
    for (std::size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == '*' || c == ':' || c == '"' || c == '<' || c == '>' ||
            c == '|' || static_cast<unsigned char>(c) < 0x20) {
            return "名字里不能有 / \\ ? # * : \" < > | 或控制字符";
        }
    }
    return {};
}

}  // namespace vat::lua_meta
