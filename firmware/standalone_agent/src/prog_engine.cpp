#include "prog_engine.h"

#include <cstdlib>
#include <cstring>

namespace vat::prog {
namespace {

const char* const kActions[] = {"read", "write", "exec", "observe", "input", "session"};
const char* const kControls[] = {"if", "repeat", "wait"};
const char* const kCompare[] = {"is", "not", "has", "above", "below"};
constexpr std::size_t kMaxLog = 40;

bool one_of(const char* key, const char* const* list, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (std::strcmp(key, list[i]) == 0) return true;
    }
    return false;
}
template <std::size_t N>
bool one_of(const char* key, const char* const (&list)[N]) { return one_of(key, list, N); }

// The action key of a step ("" when it is a control step or invalid).
String action_of(const cJSON* step) {
    for (const char* key : kActions) {
        if (cJSON_GetObjectItemCaseSensitive(step, key)) return key;
    }
    return "";
}

String text_of(const cJSON* value) {
    if (cJSON_IsString(value) && value->valuestring) return value->valuestring;
    if (cJSON_IsNumber(value)) {
        const double d = value->valuedouble;
        if (d == static_cast<double>(static_cast<long>(d))) return String(static_cast<long>(d));
        return String(d, 6);
    }
    if (cJSON_IsBool(value)) return cJSON_IsTrue(value) ? "true" : "false";
    return "";
}

bool number(const String& text, double& out) {
    String t = text;
    t.trim();
    if (t.isEmpty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0';
}

// {name} references inside `text`: a token has no spaces, quotes or braces,
// so JSON or prose in a content string is not mistaken for one.
void references(const String& text, std::vector<String>& out) {
    int from = 0;
    while (true) {
        const int open = text.indexOf('{', from);
        if (open < 0) return;
        const int close = text.indexOf('}', open + 1);
        if (close < 0) return;
        const String token = text.substring(open + 1, close);
        bool plain = !token.isEmpty() && token.length() <= 40;
        for (std::size_t i = 0; plain && i < token.length(); ++i) {
            const char c = token[i];
            plain = c != ' ' && c != '"' && c != '{' && c != '\'' && c != ':' && c != ',';
        }
        if (plain) out.push_back(token);
        from = close + 1;
    }
}

void string_refs(const cJSON* value, std::vector<String>& out) {
    if (cJSON_IsString(value)) {
        references(value->valuestring, out);
    } else if (cJSON_IsArray(value) || cJSON_IsObject(value)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, value) string_refs(item, out);
    }
}

bool valid_var(const String& name) {
    if (name.isEmpty() || name.length() > 32) return false;
    if (name == "input" || name == "event" || name == "error" || name == "now") return false;
    for (std::size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (c == '{' || c == '}' || c == '.' || c == ' ' || c == '"' || static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    return true;
}

bool valid_name(const String& name) {
    if (name.isEmpty() || name.length() > 48 || name[0] == '.') return false;
    for (std::size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == '&' || c == '{' || c == '}' || c == '"' ||
            static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    return true;
}

struct Checker {
    explicit Checker(Info& into) : info(into) {}
    Info& info;
    bool declared_inputs = false;
    std::vector<String> defined;

    bool is_defined(const String& name) const {
        for (const auto& d : defined) {
            if (d == name) return true;
        }
        return false;
    }

    String ref(const String& name) {
        if (name == "error" || name == "now" || name.startsWith("event.")) return "";
        if (name.startsWith("input.")) {
            const String key = name.substring(6);
            for (const auto& in : info.inputs) {
                if (in.name == key) return "";
            }
            if (declared_inputs) return "用了没有在 input 里声明的参数「" + key + "」";
            info.inputs.push_back({key, "", true});
            return "";
        }
        if (is_defined(name)) return "";
        return "变量「" + name + "」在使用前没有用 as 定义过";
    }

    String steps(const cJSON* list, int depth, const String& where) {
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) return where + "必须是非空的步骤列表";
        if (depth > kMaxNesting) return "嵌套超过 " + String(kMaxNesting) + " 层";
        int n = 0;
        const cJSON* step = nullptr;
        cJSON_ArrayForEach(step, list) {
            ++n;
            const String at = where + "第 " + String(n) + " 步";
            if (++info.statements > kMaxStatements) return "步骤超过 " + String(kMaxStatements) + " 个";
            if (!cJSON_IsObject(step)) return at + "必须是对象";
            int primaries = 0;
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, step) {
                if (one_of(item->string, kActions) || one_of(item->string, kControls)) ++primaries;
            }
            if (primaries != 1) {
                return at + "必须有且只有一个动作（read/write/exec/observe/input/session）或流程关键字（if/repeat/wait）";
            }
            const String err = one(step, depth, at);
            if (!err.isEmpty()) return err;
        }
        return "";
    }

    String one(const cJSON* step, int depth, const String& at) {
        const String tool = action_of(step);
        if (!tool.isEmpty()) {
            const String target = json_str(step, tool.c_str());
            if (target.isEmpty()) return at + "的 " + tool + " 要写 target（字符串）";
            const cJSON* as = cJSON_GetObjectItemCaseSensitive(step, "as");
            if (as && (!cJSON_IsString(as) || !valid_var(as->valuestring))) return at + "的 as 不是合法的变量名";
            const cJSON* on_error = cJSON_GetObjectItemCaseSensitive(step, "on_error");
            if (on_error && text_of(on_error) != "stop" && text_of(on_error) != "continue") {
                return at + "的 on_error 只能是 stop 或 continue";
            }
            std::vector<String> refs;
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, step) {
                if (std::strcmp(item->string, "as") != 0 && std::strcmp(item->string, "on_error") != 0) {
                    string_refs(item, refs);
                }
            }
            for (const auto& r : refs) {
                const String err = ref(r);
                if (!err.isEmpty()) return at + "：" + err;
            }
            if (target.startsWith("ask://")) info.uses_ask = true;
            String callee, query;
            if (tool == "exec" && program_target(target, callee, query)) {
                if (callee.indexOf('{') >= 0) return at + "：要调用的程序名不能用变量";
                bool known = false;
                for (const auto& c : info.callees) known = known || c == callee;
                if (!known) info.callees.push_back(callee);
            }
            if (as) defined.push_back(as->valuestring);
            return "";
        }
        if (cJSON_GetObjectItemCaseSensitive(step, "if")) {
            const String operand = json_str(step, "if");
            if (operand.isEmpty()) return at + "的 if 要写变量名（不加花括号）";
            const String err = ref(operand);
            if (!err.isEmpty()) return at + "：" + err;
            int comparators = 0;
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, step) {
                const char* key = item->string;
                if (one_of(key, kCompare)) {
                    ++comparators;
                } else if (std::strcmp(key, "if") != 0 && std::strcmp(key, "then") != 0 && std::strcmp(key, "else") != 0) {
                    return at + "的 if 里不认识「" + String(key) + "」（只有 is/not/has/above/below、then、else）";
                }
            }
            if (comparators != 1) return at + "的 if 要有且只有一个比较：is、not、has、above、below";
            String e = steps(cJSON_GetObjectItemCaseSensitive(step, "then"), depth + 1, at + "的 then ");
            if (!e.isEmpty()) return e;
            const cJSON* otherwise = cJSON_GetObjectItemCaseSensitive(step, "else");
            if (otherwise && !(cJSON_IsArray(otherwise) && cJSON_GetArraySize(otherwise) == 0)) {
                e = steps(otherwise, depth + 1, at + "的 else ");
            }
            return e;
        }
        if (cJSON_GetObjectItemCaseSensitive(step, "repeat")) {
            const cJSON* times = cJSON_GetObjectItemCaseSensitive(step, "repeat");
            if (!cJSON_IsNumber(times) || times->valueint < 1 || times->valueint > kMaxRepeat) {
                return at + "的 repeat 要写 1 到 " + String(kMaxRepeat) + " 的次数";
            }
            if (cJSON_GetArraySize(step) != 2) return at + "的 repeat 只能带 steps";
            return steps(cJSON_GetObjectItemCaseSensitive(step, "steps"), depth + 1, at + "的 repeat ");
        }
        const long seconds = parse_wait(json_str(step, "wait"));
        if (seconds <= 0 || seconds > kMaxWaitSeconds) return at + "的 wait 要写 1s 到 24h 的时长，如 25m";
        if (cJSON_GetArraySize(step) != 1) return at + "的 wait 不能带其他字段";
        info.uses_wait = true;
        return "";
    }
};

cJSON* resolve(cJSON* program, const String& path) {
    cJSON* node = program;
    int from = 0;
    while (node && from <= static_cast<int>(path.length())) {
        int dot = path.indexOf('.', from);
        if (dot < 0) dot = path.length();
        const String seg = path.substring(from, dot);
        if (!seg.isEmpty() && seg[0] >= '0' && seg[0] <= '9') {
            node = cJSON_GetArrayItem(node, seg.toInt());
        } else {
            node = cJSON_GetObjectItemCaseSensitive(node, seg.c_str());
        }
        from = dot + 1;
    }
    return node;
}

uint32_t fnv1a(const char* text) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

}  // namespace

// ---------------------------------------------------------------- helpers

long parse_wait(const String& text) {
    long total = 0, value = 0;
    bool digits = false, any = false;
    for (std::size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
            if (value > 1000000) return -1;
            continue;
        }
        if (c == ' ') continue;
        if (!digits) return -1;
        if (c == 's') total += value;
        else if (c == 'm') total += value * 60;
        else if (c == 'h') total += value * 3600;
        else return -1;
        value = 0;
        digits = false;
        any = true;
    }
    if (digits) return -1;  // a number without a unit
    return any ? total : -1;
}

bool program_target(const String& target, String& name, String& query) {
    if (!target.startsWith("prog://")) return false;
    String rest = target.substring(7);
    const int mark = rest.indexOf('?');
    query = mark < 0 ? String() : rest.substring(mark + 1);
    name = url_decode(mark < 0 ? rest : rest.substring(0, mark));
    while (name.endsWith("/")) name.remove(name.length() - 1);
    return !name.isEmpty() && name.indexOf('/') < 0;
}

String validate(const cJSON* program, Info& info) {
    info = Info{};
    if (!cJSON_IsObject(program)) return "程序必须是一个 JSON 对象";
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, program) {
        const char* key = item->string;
        if (std::strcmp(key, "name") != 0 && std::strcmp(key, "about") != 0 && std::strcmp(key, "input") != 0 &&
            std::strcmp(key, "steps") != 0) {
            return "不认识的字段「" + String(key) + "」：程序只有 name、about、input、steps";
        }
    }
    info.name = json_str(program, "name");
    if (!valid_name(info.name)) return "name 要写程序名（不超过 48 字节，不能含 / ? # & { } \"）";
    info.about = json_str(program, "about");
    Checker checker(info);
    const cJSON* inputs = cJSON_GetObjectItemCaseSensitive(program, "input");
    if (inputs) {
        if (!cJSON_IsObject(inputs)) return "input 要写成对象，如 {\"to\": null, \"mode\": \"walk\"}";
        checker.declared_inputs = true;
        cJSON_ArrayForEach(item, inputs) {
            if (!valid_var(item->string)) return "参数名「" + String(item->string) + "」不合法";
            if (!cJSON_IsNull(item) && !cJSON_IsString(item) && !cJSON_IsNumber(item)) {
                return "参数「" + String(item->string) + "」的默认值只能是 null（必填）、文字或数字";
            }
            info.inputs.push_back({item->string, text_of(item), cJSON_IsNull(item) != 0});
        }
    }
    return checker.steps(cJSON_GetObjectItemCaseSensitive(program, "steps"), 1, "");
}

String check_calls(const String& name, const std::vector<String>& callees, Host& host) {
    // Depth-first over the call graph; `path` is the chain from `name`.
    std::vector<String> path{name};
    std::function<String(const std::vector<String>&)> walk = [&](const std::vector<String>& next) -> String {
        for (const auto& callee : next) {
            for (const auto& p : path) {
                if (p == callee) {
                    String chain;
                    for (const auto& q : path) chain += q + " → ";
                    return "调用成环：" + chain + callee;
                }
            }
            if (static_cast<int>(path.size()) >= kMaxDepth) {
                return "调用层数超过 " + String(kMaxDepth) + "（" + path.front() + " 起）";
            }
            cJSON* program = host.load(callee);
            if (!program) return "调用的程序「" + callee + "」不存在，先建立它";
            Info info;
            validate(program, info);
            cJSON_Delete(program);
            path.push_back(callee);
            const String err = walk(info.callees);
            path.pop_back();
            if (!err.isEmpty()) return err;
        }
        return "";
    };
    return walk(callees);
}

void for_each_action(const cJSON* steps, const std::function<void(const String& tool, const cJSON* step)>& visit) {
    const cJSON* step = nullptr;
    cJSON_ArrayForEach(step, steps) {
        const String tool = action_of(step);
        if (!tool.isEmpty()) {
            visit(tool, step);
            continue;
        }
        for (const char* nested : {"then", "else", "steps"}) {
            const cJSON* list = cJSON_GetObjectItemCaseSensitive(step, nested);
            if (cJSON_IsArray(list)) for_each_action(list, visit);
        }
    }
}

cJSON* action_arguments(const cJSON* step, String& tool) {
    tool = action_of(step);
    cJSON* args = cJSON_CreateObject();
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, step) {
        const char* key = item->string;
        if (std::strcmp(key, "as") == 0 || std::strcmp(key, "on_error") == 0) continue;
        if (tool == key) {
            cJSON_AddStringToObject(args, "target", json_str(step, key).c_str());
        } else {
            cJSON_AddItemToObject(args, key, cJSON_Duplicate(item, true));
        }
    }
    // Required by the tool schemas but meaningless in a program: Policy decides
    // confirmation, writes overwrite.
    if ((tool == "write" || tool == "exec") && !cJSON_GetObjectItemCaseSensitive(args, "confirm_required")) {
        cJSON_AddBoolToObject(args, "confirm_required", false);
    }
    if (tool == "write" && !cJSON_GetObjectItemCaseSensitive(args, "mode")) {
        cJSON_AddStringToObject(args, "mode", "overwrite");
    }
    return args;
}

uint32_t fingerprint(const cJSON* step) {
    cJSON* copy = cJSON_Duplicate(step, true);
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "as");
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "on_error");
    char* printed = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    const uint32_t h = printed ? fnv1a(printed) : 0;
    cJSON_free(printed);
    return h;
}

// ---------------------------------------------------------------- Run

Run::~Run() {
    while (!frames_.empty()) drop_frame();
    cJSON_Delete(event_);
}

void Run::drop_frame() {
    cJSON_Delete(frames_.back().program);
    cJSON_Delete(frames_.back().vars);
    frames_.pop_back();
}

void Run::note(const String& line) {
    log_.push_back(utf8_head(line, 160));
    if (log_.size() > kMaxLog) log_.erase(log_.begin());
}

String Run::enter(Host& host, const String& name, const String& query) {
    cJSON* program = host.load(name);
    if (!program) return "没有程序「" + name + "」";
    Info info;
    const String err = validate(program, info);
    if (!err.isEmpty()) {
        cJSON_Delete(program);
        return "程序「" + name + "」无效：" + err;
    }
    cJSON* vars = cJSON_CreateObject();
    cJSON* input = cJSON_AddObjectToObject(vars, "input");
    for (const auto& in : info.inputs) {
        String value = target_param("?" + query, in.name.c_str());
        value = url_decode(value);
        if (value.isEmpty()) value = in.fallback;
        if (value.isEmpty() && in.required) {
            cJSON_Delete(program);
            cJSON_Delete(vars);
            return "调用「" + name + "」缺少参数 " + in.name;
        }
        cJSON_AddStringToObject(input, in.name.c_str(), value.c_str());
    }
    Frame frame;
    frame.name = name;
    frame.program = program;
    frame.vars = vars;
    frame.blocks.push_back({"steps", 0, 1});
    frames_.push_back(frame);
    return "";
}

String Run::start(Host& host, const String& name, const String& query, const cJSON* event) {
    cJSON_Delete(event_);
    event_ = cJSON_IsObject(event) ? cJSON_Duplicate(event, true) : cJSON_CreateObject();
    return enter(host, name, query);
}

String Run::lookup(const Frame& frame, const String& name, Host& host) const {
    if (name == "error") return error_;
    if (name == "now") return host.now();
    if (name.startsWith("event.")) return text_of(cJSON_GetObjectItemCaseSensitive(event_, name.substring(6).c_str()));
    if (name.startsWith("input.")) {
        return text_of(cJSON_GetObjectItemCaseSensitive(json_obj(frame.vars, "input"), name.substring(6).c_str()));
    }
    return text_of(cJSON_GetObjectItemCaseSensitive(frame.vars, name.c_str()));
}

String Run::fill(const Frame& frame, const String& text, Host& host, bool query_safe) const {
    if (text.indexOf('{') < 0) return text;
    const int query_at = query_safe ? text.indexOf('?') : -1;
    String out;
    int from = 0;
    while (true) {
        const int open = text.indexOf('{', from);
        const int close = open < 0 ? -1 : text.indexOf('}', open + 1);
        if (close < 0) break;
        std::vector<String> token;
        references(text.substring(open, close + 1), token);
        out += text.substring(from, open);
        if (token.size() != 1) {
            out += text.substring(open, close + 1);  // not a reference: keep as written
        } else {
            String value = lookup(frame, token.front(), host);
            if (query_at >= 0 && open > query_at) {  // inside a target's query: keep its shape
                for (const char c : {'&', '?', '#'}) {
                    for (int i = value.indexOf(c); i >= 0; i = value.indexOf(c)) {
                        value = value.substring(0, i) + " " + value.substring(i + 1);
                    }
                }
            }
            out += value;
        }
        from = close + 1;
    }
    out += text.substring(from);
    return out;
}

cJSON* Run::fill_value(const Frame& frame, const cJSON* value, Host& host) const {
    if (cJSON_IsString(value)) return cJSON_CreateString(fill(frame, value->valuestring, host, false).c_str());
    if (cJSON_IsArray(value) || cJSON_IsObject(value)) {
        cJSON* copy = cJSON_IsArray(value) ? cJSON_CreateArray() : cJSON_CreateObject();
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, value) {
            cJSON* filled = fill_value(frame, item, host);
            if (cJSON_IsArray(value)) cJSON_AddItemToArray(copy, filled);
            else cJSON_AddItemToObject(copy, item->string, filled);
        }
        return copy;
    }
    return cJSON_Duplicate(value, true);
}

bool Run::test(const Frame& frame, const cJSON* step, Host& host) const {
    const String value = lookup(frame, json_str(step, "if"), host);
    for (const char* key : kCompare) {
        const cJSON* expected = cJSON_GetObjectItemCaseSensitive(step, key);
        if (!expected) continue;
        String a = value, b = text_of(expected);
        a.trim();
        b.trim();
        double x = 0, y = 0;
        const bool numeric = number(a, x) && number(b, y);
        if (std::strcmp(key, "is") == 0) return numeric ? x == y : a == b;
        if (std::strcmp(key, "not") == 0) return numeric ? x != y : a != b;
        if (std::strcmp(key, "has") == 0) return a.indexOf(b) >= 0;
        if (std::strcmp(key, "above") == 0) return numeric && x > y;
        return numeric && x < y;  // below
    }
    return false;
}

bool Run::unwind(const String& reason) {
    error_ = reason;
    while (frames_.size() > 1) {
        drop_frame();
        Frame& caller = frames_.back();
        if (caller.pending_continue) {
            caller.pending_continue = false;
            caller.pending_as = "";
            caller.last = "";
            note("调用失败，按 on_error 继续：" + reason);
            return true;
        }
        caller.pending_as = "";
    }
    return false;
}

Result Run::resume(Host& host) {
    auto failed = [&](const String& reason) {
        Result r;
        r.status = Status::Failed;
        r.error = reason;
        note("失败：" + reason);
        return r;
    };
    while (!frames_.empty()) {
        Frame& frame = frames_.back();
        Block& block = frame.blocks.back();
        const cJSON* list = resolve(frame.program, block.path);
        if (!cJSON_IsArray(list)) return failed("程序「" + frame.name + "」在运行期间被修改，无法继续");
        if (block.index >= cJSON_GetArraySize(list)) {
            if (block.left > 1) {
                --block.left;
                block.index = 0;
                continue;
            }
            frame.blocks.pop_back();
            if (!frame.blocks.empty()) continue;
            const String value = frame.last;
            drop_frame();
            if (frames_.empty()) {
                Result r;
                r.value = value;
                return r;
            }
            Frame& caller = frames_.back();
            caller.last = value;
            if (!caller.pending_as.isEmpty()) {
                cJSON_DeleteItemFromObjectCaseSensitive(caller.vars, caller.pending_as.c_str());
                cJSON_AddStringToObject(caller.vars, caller.pending_as.c_str(), utf8_head(value, kMaxValueBytes).c_str());
            }
            caller.pending_as = "";
            caller.pending_continue = false;
            continue;
        }
        if (host.out_of_time()) {
            Result r;
            r.status = Status::Yielded;
            return r;
        }
        const int at = block.index++;
        const String here = block.path + "." + String(at);
        const cJSON* step = cJSON_GetArrayItem(list, at);
        if (++executed_ > kMaxExecuted) return failed("执行步数超过上限 " + String(kMaxExecuted));

        if (cJSON_GetObjectItemCaseSensitive(step, "if")) {
            const char* branch = test(frame, step, host) ? "then" : "else";
            const cJSON* next = cJSON_GetObjectItemCaseSensitive(step, branch);
            note("如果 " + json_str(step, "if") + " → " + branch);
            if (cJSON_IsArray(next) && cJSON_GetArraySize(next) > 0) frame.blocks.push_back({here + "." + branch, 0, 1});
            continue;
        }
        if (cJSON_GetObjectItemCaseSensitive(step, "repeat")) {
            const int times = cJSON_GetObjectItemCaseSensitive(step, "repeat")->valueint;
            frame.blocks.push_back({here + ".steps", 0, times});
            continue;
        }
        if (cJSON_GetObjectItemCaseSensitive(step, "wait")) {
            Result r;
            r.status = Status::Waiting;
            r.wait_s = parse_wait(json_str(step, "wait"));
            note("等待 " + json_str(step, "wait"));
            return r;
        }

        String tool;
        cJSON* templ = action_arguments(step, tool);
        cJSON* args = fill_value(frame, templ, host);
        cJSON_Delete(templ);
        const String target = fill(frame, json_str(step, tool.c_str()), host, true);
        cJSON_DeleteItemFromObjectCaseSensitive(args, "target");
        cJSON_AddStringToObject(args, "target", target.c_str());
        const String as = json_str(step, "as");
        const bool keep_going = json_str(step, "on_error") == "continue";

        String callee, query;
        if (tool == "exec" && program_target(target, callee, query)) {
            cJSON_Delete(args);
            if (static_cast<int>(frames_.size()) >= kMaxDepth) return failed("程序调用层数超过 " + String(kMaxDepth));
            frame.pending_as = as;
            frame.pending_continue = keep_going;
            note("运行 " + callee + (query.isEmpty() ? String() : "?" + query));
            const String err = enter(host, callee, query);  // `frame` may move after this
            if (!err.isEmpty()) {
                frames_.back().pending_as = "";
                frames_.back().pending_continue = false;
                error_ = err;
                if (keep_going) {
                    note("跳过：" + err);
                    continue;
                }
                return failed(err);
            }
            continue;
        }

        String out;
        const bool ok = host.call(frame.name, tool, args, step, out);
        cJSON_Delete(args);
        note(tool + " " + utf8_head(target, 60) + (ok ? " → " : " ✗ ") + utf8_head(out, 80));
        Frame& now = frames_.back();  // host.call never changes frames_, but be explicit
        if (!ok) {
            error_ = out;
            if (keep_going) {
                now.last = "";
                continue;
            }
            const String reason = "「" + now.name + "」的 " + tool + " " + utf8_head(target, 60) + " 失败：" + out;
            if (unwind(reason)) continue;
            return failed(reason);
        }
        now.last = out;
        if (!as.isEmpty()) {
            cJSON_DeleteItemFromObjectCaseSensitive(now.vars, as.c_str());
            cJSON_AddStringToObject(now.vars, as.c_str(), utf8_head(out, kMaxValueBytes).c_str());
        }
    }
    Result r;
    return r;
}

String Run::save() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* frames = cJSON_AddArrayToObject(root, "frames");
    for (const auto& f : frames_) {
        cJSON* fj = cJSON_CreateObject();
        cJSON_AddStringToObject(fj, "name", f.name.c_str());
        cJSON_AddItemToObject(fj, "vars", cJSON_Duplicate(f.vars, true));
        cJSON* blocks = cJSON_AddArrayToObject(fj, "blocks");
        for (const auto& b : f.blocks) {
            cJSON* bj = cJSON_CreateArray();
            cJSON_AddItemToArray(bj, cJSON_CreateString(b.path.c_str()));
            cJSON_AddItemToArray(bj, cJSON_CreateNumber(b.index));
            cJSON_AddItemToArray(bj, cJSON_CreateNumber(b.left));
            cJSON_AddItemToArray(blocks, bj);
        }
        cJSON_AddStringToObject(fj, "last", f.last.c_str());
        cJSON_AddStringToObject(fj, "pending_as", f.pending_as.c_str());
        cJSON_AddBoolToObject(fj, "pending_continue", f.pending_continue);
        cJSON_AddItemToArray(frames, fj);
    }
    cJSON_AddItemToObject(root, "event", cJSON_Duplicate(event_, true));
    cJSON_AddStringToObject(root, "error", error_.c_str());
    cJSON_AddNumberToObject(root, "executed", executed_);
    const String out = json_print(root);
    cJSON_Delete(root);
    return out;
}

String Run::restore(Host& host, const String& state) {
    while (!frames_.empty()) drop_frame();
    cJSON* root = cJSON_Parse(state.c_str());
    if (!root) return "运行状态无法解析";
    cJSON_Delete(event_);
    event_ = cJSON_Duplicate(json_obj(root, "event"), true);
    if (!event_) event_ = cJSON_CreateObject();
    error_ = json_str(root, "error");
    executed_ = json_int(root, "executed", 0);
    String err;
    const cJSON* fj = nullptr;
    cJSON_ArrayForEach(fj, cJSON_GetObjectItemCaseSensitive(root, "frames")) {
        Frame f;
        f.name = json_str(fj, "name");
        f.program = host.load(f.name);
        if (!f.program) {
            err = "程序「" + f.name + "」已不存在";
            break;
        }
        f.vars = cJSON_Duplicate(json_obj(fj, "vars"), true);
        if (!f.vars) f.vars = cJSON_CreateObject();
        const cJSON* bj = nullptr;
        cJSON_ArrayForEach(bj, cJSON_GetObjectItemCaseSensitive(fj, "blocks")) {
            f.blocks.push_back({text_of(cJSON_GetArrayItem(bj, 0)), cJSON_GetArrayItem(bj, 1)->valueint,
                                cJSON_GetArrayItem(bj, 2)->valueint});
        }
        f.last = json_str(fj, "last");
        f.pending_as = json_str(fj, "pending_as");
        f.pending_continue = json_bool(fj, "pending_continue", false);
        frames_.push_back(f);
        if (frames_.back().blocks.empty()) {
            err = "运行状态缺少位置";
            break;
        }
    }
    cJSON_Delete(root);
    if (err.isEmpty() && frames_.empty()) err = "运行状态是空的";
    if (!err.isEmpty()) {
        while (!frames_.empty()) drop_frame();
    }
    return err;
}

}  // namespace vat::prog
