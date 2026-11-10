#include "lua_vm.h"

#include <esp_heap_caps.h>
#include <multi_heap.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "console.h"
#include "policy.h"
#include "registry.h"
#include "third_party/lua/lauxlib.h"
#include "third_party/lua/lua.h"
#include "third_party/lua/lualib.h"
#include "worker.h"

namespace vat::luavm {
namespace {

constexpr int kMaxArenas = 4;

struct Arena {
    uint8_t* block = nullptr;
    std::size_t size = 0;
    multi_heap_handle_t heap = nullptr;
};

struct Context {
    Arena arenas[kMaxArenas];
    int count = 0;
    std::size_t taken = 0;  // bytes of the heap held in arenas
    std::size_t used = 0;
    std::size_t peak = 0;
    uint32_t started = 0;
    uint32_t thousands = 0;  // instructions run, in thousands
    String* output = nullptr;
    const Options* options = nullptr;
    bool canvas = false;                  // Script: per-frame budget, no tool calls
    std::size_t first_arena = kFirstArena;
};

int depth = 0;  // nested runs (a program calling a program); one task only

Context& context(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return *static_cast<Context*>(ud);
}

// ---- the private heap

bool grow(Context& c, std::size_t need) {
    if (c.count == kMaxArenas) return false;
    std::size_t size = c.count == 0 ? c.first_arena : std::max(kGrowArena, need + 1024);
    size = std::min(size, kMemoryLimit - c.taken);
    if (size < need + 512) return false;
    // A fragmented heap may not have the whole block: settle for less (the
    // VM grows by more blocks later).  The largest free block is no promise
    // (TLSF rounds requests up to a size class), so try and shrink by a
    // quarter until it fits, down to what this request needs.
    const std::size_t floor = std::max<std::size_t>(need + 512, 8192);
    uint8_t* block = nullptr;
    while (size >= floor && !(block = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)))) {
        size = size * 3 / 4;
    }
    if (!block) return false;
    multi_heap_handle_t heap = multi_heap_register(block, size);
    if (!heap) {
        heap_caps_free(block);
        return false;
    }
    c.arenas[c.count++] = {block, size, heap};
    c.taken += size;
    return true;
}

Arena* arena_of(Context& c, void* p) {
    auto* q = static_cast<uint8_t*>(p);
    for (int i = 0; i < c.count; ++i) {
        if (q >= c.arenas[i].block && q < c.arenas[i].block + c.arenas[i].size) return &c.arenas[i];
    }
    return nullptr;
}

void* arena_malloc(Context& c, std::size_t size) {
    for (int i = 0; i < c.count; ++i) {
        if (void* p = multi_heap_malloc(c.arenas[i].heap, size)) return p;
    }
    if (!grow(c, size)) return nullptr;
    return multi_heap_malloc(c.arenas[c.count - 1].heap, size);
}

void* allocate(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto& c = *static_cast<Context*>(ud);
    const std::size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        if (ptr) {
            if (Arena* a = arena_of(c, ptr)) multi_heap_free(a->heap, ptr);
            c.used -= old;
        }
        return nullptr;
    }
    void* p = nullptr;
    if (!ptr) {
        p = arena_malloc(c, nsize);
    } else {
        Arena* a = arena_of(c, ptr);
        if (!a) return nullptr;
        p = multi_heap_realloc(a->heap, ptr, nsize);
        if (!p) {  // move to another arena
            p = arena_malloc(c, nsize);
            if (!p) return nullptr;
            std::memcpy(p, ptr, std::min(old, static_cast<std::size_t>(nsize)));
            multi_heap_free(a->heap, ptr);
        }
    }
    if (!p) return nullptr;
    c.used = c.used - old + nsize;
    c.peak = std::max(c.peak, c.used);
    return p;
}

void release(Context& c) {
    for (int i = 0; i < c.count; ++i) heap_caps_free(c.arenas[i].block);
    c.count = 0;
}

// ---- limits

bool foreground(const Context& c) { return c.options && c.options->foreground; }

void check_time(lua_State* L) {
    auto& c = context(L);
    if (cancel::requested()) luaL_error(L, "已中断");
    if (foreground(c)) {  // the user is watching and can interrupt: a long run is fine
        worker::keep_alive();
        if (millis() - c.started > kForegroundMs) luaL_error(L, "运行超过 30 分钟");
        return;
    }
    if (millis() - c.started > kMaxMs) luaL_error(L, "运行超过 %d 秒", static_cast<int>(kMaxMs / 1000));
}

void hook(lua_State* L, lua_Debug*) {
    auto& c = context(L);
    ++c.thousands;
    if (c.canvas) {  // the UI task: a frame's budget, nothing else
        if (c.thousands * 1000ULL > kFrameInstructions) luaL_error(L, "一帧的计算超过上限");
        return;
    }
    if (c.thousands % 50 == 0) vTaskDelay(1);  // let the idle task run
    if (c.thousands * 1000ULL > (foreground(c) ? kForegroundInstructions : kMaxInstructions)) luaL_error(L, "执行步数超过上限");
    check_time(L);
}

void append_output(lua_State* L, const String& text, bool error = false) {
    auto& c = context(L);
    if (foreground(c)) console::write(text, error);
    String& out = *c.output;
    if (out.length() >= kMaxOutput) return;
    out += utf8_head(text, kMaxOutput - out.length());
}

String joined(lua_State* L, int from) {  // io.write's arguments, as text
    String text;
    for (int i = from; i <= lua_gettop(L); ++i) {
        text += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    return text;
}

int io_write(lua_State* L) {  // io.write(...): no newline
    check_time(L);
    append_output(L, joined(L, 1));
    return 0;
}

int io_stderr_write(lua_State* L) {  // io.stderr:write(...): the warning colour
    check_time(L);
    append_output(L, joined(L, 2), true);
    return 0;
}

// A line from the keyboard (or the run's stdin): the string; nil at the end of
// the stdin text; nil, why in a background run.  ` interrupts the program.
int read_input(lua_State* L, const String& prompt, const char* format) {
    check_time(L);
    if (!foreground(context(L))) {
        lua_pushnil(L);
        lua_pushstring(L, "后台运行没有键盘输入（用 vat.interactive() 判断）");
        return 2;
    }
    String line;
    const auto got = console::read_line(prompt, line);
    if (got == console::Got::Cancelled) return luaL_error(L, "已中断");
    if (got != console::Got::Value) {
        lua_pushnil(L);
        return 1;
    }
    while (format && *format == '*') ++format;  // Lua 5.1's "*l" / "*n"
    if (format && *format == 'n') {
        if (lua_stringtonumber(L, line.c_str()) == 0) lua_pushnil(L);
        return 1;
    }
    if (format && *format == 'L') line += "\n";
    lua_pushstring(L, line.c_str());
    return 1;
}

int io_read(lua_State* L) {  // io.read([format]): "l" (default), "L", "n"
    return read_input(L, String(), lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "l");
}

int lua_readline(lua_State* L) {  // vat.readline([prompt])
    return read_input(L, luaL_optstring(L, 1, ""), "l");
}

int lua_key(lua_State* L) {  // vat.key([timeout_ms]): one key, nil on timeout
    check_time(L);
    const lua_Integer timeout = luaL_optinteger(L, 1, 0);
    if (!foreground(context(L))) {
        lua_pushnil(L);
        lua_pushstring(L, "后台运行没有键盘输入");
        return 2;
    }
    String key;
    const auto got = console::read_key(timeout > 0 ? static_cast<uint32_t>(timeout) : 0, key);
    if (got == console::Got::Cancelled) return luaL_error(L, "已中断");
    if (got != console::Got::Value) lua_pushnil(L);
    else lua_pushstring(L, key.c_str());
    return 1;
}

int lua_clear(lua_State* L) {  // vat.clear(): empties the console
    if (foreground(context(L))) console::clear();
    return 0;
}

int lua_interactive(lua_State* L) {  // vat.interactive(): someone is at the keyboard
    lua_pushboolean(L, foreground(context(L)));
    return 1;
}

int lua_print(lua_State* L) {
    const int n = lua_gettop(L);
    String line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += "\t";
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    append_output(L, line + "\n");
    return 0;
}

// ---- JSON <-> Lua

// A cJSON tree owned for the length of a binding (freed on errors too: the
// bindings run as C++ and Lua errors are exceptions).
struct Json {
    cJSON* p;
    explicit Json(cJSON* item) : p(item) {}
    Json(Json&& other) noexcept : p(other.p) { other.p = nullptr; }
    ~Json() { cJSON_Delete(p); }
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
};

bool is_array(lua_State* L, int index) {
    const lua_Integer n = luaL_len(L, index);
    lua_Integer keys = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pop(L, 1);
        ++keys;
    }
    return keys == n;  // {} is an empty array
}

cJSON* to_json(lua_State* L, int index, int depth_now) {
    index = lua_absindex(L, index);
    if (depth_now > 8) luaL_error(L, "表嵌套太深");
    switch (lua_type(L, index)) {
        case LUA_TNIL: return cJSON_CreateNull();
        case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, index));
        case LUA_TNUMBER:
            return lua_isinteger(L, index) ? cJSON_CreateNumber(static_cast<double>(lua_tointeger(L, index)))
                                           : cJSON_CreateNumber(lua_tonumber(L, index));
        case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, index));
        case LUA_TTABLE: {
            Json out(is_array(L, index) ? cJSON_CreateArray() : cJSON_CreateObject());
            if (cJSON_IsArray(out.p)) {
                const lua_Integer n = luaL_len(L, index);
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_geti(L, index, i);
                    cJSON_AddItemToArray(out.p, to_json(L, -1, depth_now + 1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        cJSON_AddItemToObject(out.p, lua_tostring(L, -2), to_json(L, -1, depth_now + 1));
                    }
                    lua_pop(L, 1);
                }
            }
            cJSON* done = out.p;
            out.p = nullptr;
            return done;
        }
        default: return cJSON_CreateString(luaL_tolstring(L, index, nullptr));
    }
}

void push_json(lua_State* L, const cJSON* item, int depth_now) {
    luaL_checkstack(L, 4, "JSON 嵌套太深");
    if (depth_now > 16) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        const double v = item->valuedouble;
        if (v == static_cast<double>(static_cast<lua_Integer>(v)) && std::abs(v) < 9.0e15) {
            lua_pushinteger(L, static_cast<lua_Integer>(v));
        } else {
            lua_pushnumber(L, v);
        }
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring);
    } else if (cJSON_IsArray(item)) {
        lua_createtable(L, cJSON_GetArraySize(item), 0);
        int i = 0;
        const cJSON* child = nullptr;
        cJSON_ArrayForEach(child, item) {
            push_json(L, child, depth_now + 1);
            lua_rawseti(L, -2, ++i);
        }
    } else if (cJSON_IsObject(item)) {
        lua_createtable(L, 0, cJSON_GetArraySize(item));
        const cJSON* child = nullptr;
        cJSON_ArrayForEach(child, item) {
            push_json(L, child, depth_now + 1);
            lua_setfield(L, -2, child->string);
        }
    } else {
        lua_pushnil(L);
    }
}

int lua_json(lua_State* L) {  // vat.json(text) -> value, or nil, error
    Json parsed(cJSON_Parse(luaL_checkstring(L, 1)));
    if (!parsed.p) {
        lua_pushnil(L);
        lua_pushstring(L, "不是 JSON");
        return 2;
    }
    push_json(L, parsed.p, 0);
    return 1;
}

int lua_tojson(lua_State* L) {  // vat.tojson(value) -> text
    Json item(to_json(L, 1, 0));
    lua_pushstring(L, json_print(item.p).c_str());
    return 1;
}

int lua_urlencode(lua_State* L) {  // vat.urlencode(text)
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ch = s[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            luaL_addchar(&b, ch);
        } else {
            luaL_addchar(&b, '%');
            luaL_addchar(&b, kHex[ch >> 4]);
            luaL_addchar(&b, kHex[ch & 15]);
        }
    }
    luaL_pushresult(&b);
    return 1;
}

// ---- tool calls

void add_options(lua_State* L, int index, cJSON* args) {
    if (lua_isnoneornil(L, index)) return;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(L, -2);
            if (std::strcmp(key, "target") && std::strcmp(key, "confirm_required")) {
                cJSON_DeleteItemFromObject(args, key);
                cJSON_AddItemToObject(args, key, to_json(L, -1, 1));
            }
        }
        lua_pop(L, 1);
    }
}

// One tool call as the model would make it.  Policy decides; a call it would
// ask about runs only when the program's confirmed @uses cover it; secrets
// are never asked for.
int call_tool(lua_State* L, const char* tool, cJSON* args) {
    check_time(L);
    const String target = json_str(args, "target");
    if (target.startsWith("lua://")) {
        lua_pushnil(L);
        lua_pushstring(L, "Lua 程序里不能运行 lua://");
        return 2;
    }
    const auto verdict = policy::evaluate(tool, args);
    const auto* uses = context(L).options->uses;
    bool approved = false;
    String error;
    if (verdict.decision == policy::Decision::Deny) {
        error = "已拒绝：" + verdict.reason;
    } else if (verdict.decision == policy::Decision::Ask) {
        approved = uses && lua_meta::covers(*uses, tool, target);
        if (!approved) {
            error = String("需要确认：在程序开头声明 -- @uses ") + tool + " " + target + "，保存时由用户确认（" +
                    verdict.reason + "）";
        }
    }
    if (error.isEmpty() && (target.startsWith("http://") || target.startsWith("https://")) &&
        !(uses && lua_meta::covers(*uses, tool, target))) {
        // A read is allowed for the agent, but a program could carry data off in
        // a URL: each site it talks to is confirmed once, like any write.
        error = String("联网要在程序开头声明 -- @uses ") + tool + " " + target +
                "（可用 * 结尾，如 https://api.example.com/*），保存时由用户确认";
    }
    if (error.isEmpty() && !registry::needs_secret(tool, args).isEmpty()) error = "需要输入密钥，程序里不能输入：先让肥鱼调用一次";
    if (!error.isEmpty()) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    const String out = registry::execute(tool, args, approved);
    if (out.startsWith("ERROR") || out.startsWith("没有能力包可处理")) {
        lua_pushnil(L);
        lua_pushstring(L, out.c_str());
        return 2;
    }
    if (std::strcmp(tool, "exec") == 0 && target.startsWith("prog://")) {
        // Another program: hand back what it returned, not the sentence around it.
        static const char kDone[] = "完成，结果：";
        const int at = out.indexOf(kDone);
        if (at >= 0) {
            String value = out.substring(at + strlen(kDone));
            const int tail = value.indexOf("\n输出：");
            if (tail >= 0) value = value.substring(0, tail);
            if (value == "（无）") value = String();
            lua_pushstring(L, value.c_str());
            return 1;
        }
    }
    lua_pushstring(L, out.c_str());
    return 1;
}

Json target_args(lua_State* L) {
    const char* target = luaL_checkstring(L, 1);
    Json args(cJSON_CreateObject());
    cJSON_AddStringToObject(args.p, "target", target);
    return args;
}

void add_content(lua_State* L, int index, cJSON* args, const char* key) {
    if (lua_type(L, index) == LUA_TTABLE) {
        Json content(to_json(L, index, 0));
        cJSON_AddStringToObject(args, key, json_print(content.p).c_str());
    } else {
        cJSON_AddStringToObject(args, key, luaL_optstring(L, index, ""));
    }
}

int lua_read(lua_State* L) {  // vat.read(target[, options])
    Json args = target_args(L);
    add_options(L, 2, args.p);
    return call_tool(L, "read", args.p);
}

int lua_write(lua_State* L) {  // vat.write(target, content[, options]); a table is sent as JSON
    Json args = target_args(L);
    add_content(L, 2, args.p, "content");
    cJSON_AddStringToObject(args.p, "mode", "overwrite");
    add_options(L, 3, args.p);
    return call_tool(L, "write", args.p);
}

int lua_exec(lua_State* L) {  // vat.exec(target[, program[, {args…}[, options]]])
    Json args = target_args(L);
    cJSON_AddStringToObject(args.p, "program", luaL_optstring(L, 2, "run"));
    cJSON* list = cJSON_AddArrayToObject(args.p, "args");
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        const lua_Integer n = luaL_len(L, 3);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, 3, i);
            cJSON_AddItemToArray(list, cJSON_CreateString(luaL_tolstring(L, -1, nullptr)));
            lua_pop(L, 2);
        }
    }
    add_options(L, 4, args.p);
    return call_tool(L, "exec", args.p);
}

int lua_observe(lua_State* L) {  // vat.observe(target[, options])
    Json args = target_args(L);
    add_options(L, 2, args.p);
    return call_tool(L, "observe", args.p);
}

int lua_input(lua_State* L) {  // vat.input(target, text[, options]): options.keys, options.submit
    Json args = target_args(L);
    cJSON_AddStringToObject(args.p, "text", luaL_optstring(L, 2, ""));
    cJSON_AddBoolToObject(args.p, "submit", false);
    add_options(L, 3, args.p);
    return call_tool(L, "input", args.p);
}

int lua_session(lua_State* L) {  // vat.session(action[, options])
    Json args(cJSON_CreateObject());
    cJSON_AddStringToObject(args.p, "action", luaL_checkstring(L, 1));
    add_options(L, 2, args.p);
    return call_tool(L, "session", args.p);
}

int lua_after(lua_State* L) {  // vat.after("25m", "prog://名字?参数"): runs that program later, once
    const String delay = luaL_checkstring(L, 1);
    const String run = luaL_checkstring(L, 2);
    if (!run.startsWith("prog://")) return luaL_error(L, "vat.after 只能预约运行程序（prog://名字）");
    Json content(cJSON_CreateObject());
    cJSON_AddStringToObject(content.p, "when", ("time://in/" + delay).c_str());
    cJSON_AddStringToObject(content.p, "run", run.c_str());
    Json args(cJSON_CreateObject());
    cJSON_AddStringToObject(args.p, "target", "trigger://new");
    cJSON_AddStringToObject(args.p, "content", json_print(content.p).c_str());
    cJSON_AddStringToObject(args.p, "mode", "create");
    return call_tool(L, "write", args.p);
}

int lua_stream(lua_State* L) {  // vat.stream(target) -> {x = …, …}, or nil, error
    const String target = luaL_checkstring(L, 1);
    String out;
    if (!registry::read_stream(target, out)) {
        lua_pushnil(L);
        lua_pushstring(L, out.isEmpty() ? "不是可读取的流" : out.c_str());
        return 2;
    }
    Json parsed(cJSON_Parse(out.c_str()));
    if (!parsed.p) {
        lua_pushstring(L, out.c_str());
        return 1;
    }
    push_json(L, parsed.p, 0);
    return 1;
}

int lua_sleep(lua_State* L) {  // vat.sleep(ms), at most 60 s a call
    const lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0 || ms > static_cast<lua_Integer>(kMaxSleepMs)) {
        return luaL_error(L, "sleep 只能 0 到 %d 毫秒；更久用 vat.after", static_cast<int>(kMaxSleepMs));
    }
    const uint32_t until = millis() + static_cast<uint32_t>(ms);
    while (static_cast<int32_t>(until - millis()) > 0) {
        check_time(L);
        delay(std::min<uint32_t>(50, until - millis()));
    }
    return 0;
}

int lua_millis(lua_State* L) {
    lua_pushinteger(L, millis());
    return 1;
}

const luaL_Reg kVat[] = {{"read", lua_read},       {"write", lua_write},         {"exec", lua_exec},
                         {"observe", lua_observe}, {"input", lua_input},         {"session", lua_session},
                         {"stream", lua_stream},   {"after", lua_after},         {"json", lua_json},
                         {"tojson", lua_tojson},   {"urlencode", lua_urlencode}, {"sleep", lua_sleep},
                         {"millis", lua_millis},   {"log", lua_print},           {"readline", lua_readline},
                         {"key", lua_key},         {"clear", lua_clear},         {"interactive", lua_interactive},
                         {nullptr, nullptr}};

const luaL_Reg kCanvasVat[] = {{"stream", lua_stream}, {"json", lua_json},   {"tojson", lua_tojson},
                               {"millis", lua_millis}, {"log", lua_print}, {nullptr, nullptr}};

void open_libraries(lua_State* L, bool canvas = false) {
    static const luaL_Reg kLibs[] = {{LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine},
                                     {LUA_MATHLIBNAME, luaopen_math}, {LUA_STRLIBNAME, luaopen_string},
                                     {LUA_TABLIBNAME, luaopen_table}, {LUA_UTF8LIBNAME, luaopen_utf8},
                                     {nullptr, nullptr}};
    for (const luaL_Reg* lib = kLibs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    for (const char* name : {"dofile", "loadfile", "load"}) {  // no files, no bytecode
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_pushcfunction(L, lua_print);
    lua_setglobal(L, "print");
    if (canvas) luaL_newlib(L, kCanvasVat);
    else luaL_newlib(L, kVat);
    lua_setglobal(L, "vat");
    if (canvas) return;
    static const luaL_Reg kIo[] = {{"write", io_write}, {"read", io_read}, {nullptr, nullptr}};
    luaL_newlib(L, kIo);  // the console's standard I/O; no files
    lua_newtable(L);
    lua_pushcfunction(L, io_stderr_write);
    lua_setfield(L, -2, "write");
    lua_setfield(L, -2, "stderr");
    lua_setglobal(L, "io");
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// (entry name) -> what entry(input.target, input) returned, a table as JSON.
int call_entry(lua_State* L) {
    const char* entry = lua_tostring(L, 1);
    if (lua_getglobal(L, entry) != LUA_TFUNCTION) {
        return luaL_error(L, "这个包没有 %s 函数（写成全局函数 function %s(target, args) … end）", entry, entry);
    }
    lua_getglobal(L, "input");
    lua_getfield(L, -1, "target");
    lua_insert(L, -2);
    lua_call(L, 2, 1);
    if (lua_istable(L, -1)) {
        Json value(to_json(L, -1, 0));
        lua_pushstring(L, json_print(value.p).c_str());
    }
    return 1;
}

}  // namespace

bool run(const String& source, const String& name, const Options& options, String& output, String& result,
         String& error, Stats& stats) {
    if (depth >= kMaxDepth) {
        error = "程序互相调用超过 " + String(kMaxDepth) + " 层";
        return false;
    }
    Context c;
    c.started = millis();
    c.output = &output;
    c.options = &options;
    lua_State* L = lua_newstate(allocate, &c);
    if (!L) {
        release(c);
        error = "内存紧张，现在无法运行 Lua 程序，稍后再试";
        return false;
    }
    ++depth;
    bool ok = false;
    try {
        open_libraries(L);
        if (options.input) push_json(L, options.input, 0);
        else lua_newtable(L);
        lua_setglobal(L, "input");
        lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
        lua_pushcfunction(L, traceback);
        const String chunk = "=" + name;
        if (luaL_loadbufferx(L, source.c_str(), source.length(), chunk.c_str(), "t") != LUA_OK) {
            error = String("语法错误：") + lua_tostring(L, -1);
        } else if (lua_pcall(L, 0, 1, -2) != LUA_OK) {
            error = lua_tostring(L, -1) ? String(lua_tostring(L, -1)) : String("运行出错");
        } else if (options.entry) {
            lua_pop(L, 1);  // the chunk's own return value
            lua_pushcfunction(L, call_entry);
            lua_pushstring(L, options.entry);
            if (lua_pcall(L, 1, 1, -3) != LUA_OK) {
                error = lua_tostring(L, -1) ? String(lua_tostring(L, -1)) : String("运行出错");
            } else {
                if (!lua_isnil(L, -1)) result = luaL_tolstring(L, -1, nullptr);
                ok = true;
            }
        } else {
            if (!lua_isnil(L, -1)) result = luaL_tolstring(L, -1, nullptr);
            ok = true;
        }
    } catch (...) {  // outside a protected call: no memory to open the libraries
        error = "Lua 内存不足";
    }
    --depth;
    stats.peak = c.peak;
    stats.arena = c.taken;
    stats.instructions = c.thousands * 1000;
    stats.ms = millis() - c.started;
    lua_close(L);
    release(c);
    return ok;
}

String compile(const String& source, const String& name) {
    Context c;
    c.started = millis();
    String sink;
    Options none;
    c.output = &sink;
    c.options = &none;
    lua_State* L = lua_newstate(allocate, &c);
    if (!L) {
        release(c);
        return "内存不足，无法检查语法";
    }
    String error;
    try {
        const String chunk = "=" + name;
        if (luaL_loadbufferx(L, source.c_str(), source.length(), chunk.c_str(), "t") != LUA_OK) {
            error = lua_tostring(L, -1);
        }
    } catch (...) {
        error = "Lua 内存不足";
    }
    lua_close(L);
    release(c);
    return error;
}


// ---------------------------------------------------------------- Script

namespace {
struct ScriptContext {
    Context c;
    String output;  // print() in an update: kept, never shown
    Options none;
};
constexpr std::size_t kScriptArena = 12 * 1024;  // canvas scripts are small
}  // namespace

String Script::open(const String& vars_json, const String& update) {
    close();
    auto* sc = new ScriptContext();
    sc->c.canvas = true;
    sc->c.first_arena = kScriptArena;
    sc->c.started = millis();
    sc->c.output = &sc->output;
    sc->c.options = &sc->none;
    lua_State* L = lua_newstate(allocate, &sc->c);
    if (!L) {
        release(sc->c);
        delete sc;
        return "内存紧张，画布的 update 没法运行";
    }
    state_ = L;
    context_ = sc;
    String error;
    try {
        open_libraries(L, true);
        lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
        Json vars(cJSON_Parse(vars_json.c_str()));
        const cJSON* v = nullptr;
        cJSON_ArrayForEach(v, vars.p) {
            push_json(L, v, 0);
            lua_setglobal(L, v->string);
        }
        if (!update.isEmpty()) {
            if (luaL_loadbufferx(L, update.c_str(), update.length(), "=update", "t") != LUA_OK) {
                error = String("update 语法错误：") + lua_tostring(L, -1);
                lua_pop(L, 1);
            } else {
                update_ = luaL_ref(L, LUA_REGISTRYINDEX);
            }
        }
    } catch (...) {
        error = "画布的 update 内存不足";
    }
    if (!error.isEmpty()) close();
    return error;
}

String Script::step(long t) {
    auto* L = static_cast<lua_State*>(state_);
    if (!L) return {};
    auto* sc = static_cast<ScriptContext*>(context_);
    String error;
    try {
        lua_pushinteger(L, t);
        lua_setglobal(L, "t");
        if (update_ == LUA_NOREF || update_ == -2) return {};
        sc->c.thousands = 0;
        lua_rawgeti(L, LUA_REGISTRYINDEX, update_);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            error = lua_tostring(L, -1) ? String(lua_tostring(L, -1)) : String("update 出错");
            lua_pop(L, 1);
        }
    } catch (...) {
        error = "update 内存不足";
    }
    sc->output = String();  // never shown: keep it from growing
    return error;
}

bool Script::text(const String& name, String& out) {
    auto* L = static_cast<lua_State*>(state_);
    if (!L) return false;
    bool found = true;
    try {
        lua_getglobal(L, name.c_str());
        if (lua_isinteger(L, -1)) {
            out = String(static_cast<long>(lua_tointeger(L, -1)));
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            char text[32];
            snprintf(text, sizeof(text), "%.6f", lua_tonumber(L, -1));
            String s = text;
            while (s.endsWith("0")) s.remove(s.length() - 1);
            if (s.endsWith(".")) s.remove(s.length() - 1);
            if (s == "-0") s = "0";
            out = s;
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            out = lua_tostring(L, -1);
        } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
            out = lua_toboolean(L, -1) ? "1" : "0";
        } else {
            found = false;
        }
        lua_pop(L, 1);
    } catch (...) {
        found = false;
    }
    return found;
}

void Script::close() {
    auto* sc = static_cast<ScriptContext*>(context_);
    if (state_) lua_close(static_cast<lua_State*>(state_));
    if (sc) {
        release(sc->c);
        delete sc;
    }
    state_ = nullptr;
    context_ = nullptr;
    update_ = -2;
}

}  // namespace vat::luavm
