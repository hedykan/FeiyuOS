// gfx: drawing on the screen (docs/V075_PLAN.zh-CN.md G2, G2b).
//   write gfx://canvas/<名字>        content: canvas JSON (ui_canvas.h); shows it
//   write gfx://canvas/<名字>/close
//   read  gfx://canvas/<名字>        whether it is on screen
//   write gfx://frame                content: one frame, an array of ops; shows it at once
//   write gfx://frame/close
//   read  gfx://help                 every function (gfx_api), with its parameters and an example
//   read  gfx://screen               size and theme colours
//   read  gfx://measure?text=…&size=…  a text's width in pixels
// Programs that declare "-- @uses gfx" get M5GFX's drawing calls by their own
// names and arguments (gfx.fillRect(…), gfx.setTextColor(…), gfx.print(…)…),
// then gfx.present(): the ops of a frame go to the screen together, the same
// as one write to gfx://frame and under the same policy.  The compositor
// (ui_compose) draws them band by band: a frame never shows half drawn.
#include <cstdio>
#include <cstring>

#include "../common.h"
#include "../gfx_api.h"
#include "../gfx_exec.h"
#include "../lua_vm.h"
#include "../policy.h"
#include "../registry.h"
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lua.h"
#include "../ui_canvas.h"
#include "../ui_kit.h"
#include "../ui_live.h"
#include "../ui_panel.h"
#include "../ui_state.h"

namespace vat::registry {
namespace {

using ui::Gfx;
using ui::Overlay;
using ui_canvas::Op;

constexpr char kFrameName[] = "frame";  // the overlay a program's frames show in
constexpr unsigned long kPaceMs = 100;  // present() waits at most this long for the UI to take the last frame

constexpr char kUsage[] =
    "画图（gfx://）：屏幕 240×135。函数与 M5GFX 同名同参（fillRect、drawString、setTextColor、print、drawPng……，"
    "全部函数、参数和例子 read gfx://help）。画一张图或一个会动的画面：write target=gfx://canvas/名字，content 为 JSON 对象，"
    "ops 为指令数组，每条写成 [函数名, 参数…]，如 [fillRect, 10, 10, 50, 20, red]，颜色写 #RRGGBB、white red cyan 这类名字或 RGB565 数字，"
    "最多 64 条；FeiyuOS 另有 progressBar（x y 宽 高 百分比 颜色 底色）、drawQr（文字 x y 宽）、drawImage（/vat/ 下图片 x y，最多一张）、"
    "drawString 第 4 个参数可写 {size, color, datum} 只作用于这一条、[rotate, 角度, 中心x, 中心y] 转动其后的形状；关闭 write gfx://canvas/名字/close。"
    "动画在 content 顶层加 animate：every 每帧间隔如 100ms（至少 33ms）、frames 帧数（0 表示一直到关闭）、step 每帧增量，"
    "参数里的 {t} 换成 帧序号乘 step（{t} {1} {1#} 这类占位符在 JSON 里写成带引号的字符串），例如椭圆原地旋转：ops 先 [rotate, {t}, 120, 67] 再 [fillEllipse, 120, 67, 50, 20, cyan]，"
    "animate 为 every 100ms、frames 0、step 10。"
    "变量动画：顶层加 vars（初值对象，如 x 为 20、vx 为 6）和 update（每帧先执行的一段 Lua 代码，如 x = x + vx  if x > 220 or x < 20 then vx = -vx end；可用 vars 里的变量、t 帧序号乘 step、math，以及 vat.stream 读传感器，不能调用其他能力），参数里写 {x} 取变量值；有 update 没 animate 时每 33ms 一帧直到关闭。"
    "可加 source 定时刷新（同面板），参数里的 {1} {2} 换成结果的第 1、2 行，{1#} 换成第 1 行里的第一个数字（用于大数字和进度条）。画布能存成本地应用（ui://apps/名字，content 同上）。"
    "程序里逐帧画：开头写 -- @uses gfx，然后 gfx.fillScreen('black')、gfx.fillRect(x, y, 宽, 高, 颜色)、gfx.setTextColor('white')、gfx.drawString(文字, x, y) 等（同 M5GFX，状态如字体、颜色、光标在下一帧继续有效），"
    "画完一帧调用 gfx.present() 一次上屏（不调用屏幕不变）；查询 gfx.width()、gfx.textWidth(文字)、gfx.fontHeight()、gfx.getCursorX() 立即返回；"
    "颜色常量 gfx.TFT_RED 这类，gfx.printf 同 string.format；gfx.close() 收起画面。"
    "也可以 write gfx://frame，content 为一帧的指令数组。read gfx://screen 看屏幕尺寸和配色。";

// The theme (ui_kit) as colour names a program can use.
constexpr char kScreen[] =
    "屏幕 240×135 像素，左上角为 0,0。主题色：背景 #0F1216，面板 #1A1F26，文字 #E6EAF0，次要 #7A8494，"
    "强调 #3FB8AF，警告 #E8A33D，危险 #E5534B。文字 size 1 时中文 12×12、英文宽 6，size 2 到 4 按倍数放大。";

// One frame to the screen; files and bitmaps are read here (worker), so the UI only draws.
String show_frame(std::vector<Op>&& ops, bool raise) {
    Overlay overlay;
    overlay.kind = Overlay::Kind::Canvas;
    overlay.panel = kFrameName;
    overlay.canvas = std::move(ops);
    overlay.raise = raise;
    String error;
    if (!gfx_exec::prepare(overlay.canvas, overlay.blobs, error)) return error;
    ui::post_overlay(std::move(overlay));
    return {};
}

String frame_policy() {  // gfx.present() asks the same as a write to gfx://frame
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "target", "gfx://frame");
    cJSON_AddBoolToObject(args, "confirm_required", false);
    const auto verdict = policy::evaluate("write", args);
    cJSON_Delete(args);
    return verdict.decision == policy::Decision::Allow ? String() : verdict.reason;
}

// ---------------------------------------------------------------- Lua: gfx.*

// What a program's frames carry.  State (fonts, colours, the cursor…) lasts
// across frames as on M5GFX's screen: a frame starts with the state the last
// one ended with.  The ruler (1-bit, 4 KB, made on first use) runs the
// program's state and text calls as they come, so queries answer at once.
struct Frame {
    std::vector<Op> ops;
    int images = 0;
    std::size_t bytes = 0;
    bool shown = false;                         // a frame of this run is up: the next ones do not pull the page back
    std::vector<std::pair<String, Op>> state;   // the last op of each kind of state, in first-set order
    std::vector<Op> prefix;                     // the state this frame starts from
    lgfx::LGFX_Sprite* ruler = nullptr;
};
constexpr char kFrameKey[] = "vat.gfx.frame";

Frame& frame_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kFrameKey);
    Frame* frame = *static_cast<Frame**>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *frame;
}

int frame_gc(lua_State* L) {
    Frame** box = static_cast<Frame**>(lua_touserdata(L, 1));
    if (*box && (*box)->ruler) {
        (*box)->ruler->deleteSprite();
        delete (*box)->ruler;
    }
    delete *box;
    *box = nullptr;
    return 0;
}

Gfx* ruler(Frame& frame) {
    if (!frame.ruler) {
        auto* r = new lgfx::LGFX_Sprite();
        r->setColorDepth(1);
        if (!r->createSprite(ui::kWidth, ui::kHeight)) {
            delete r;
            return nullptr;
        }
        gfx_exec::reset(*r);
        frame.ruler = r;
    }
    return frame.ruler;
}

String state_slot(const String& name) {
    if (name == "setFont" || name == "setTextFont") return "font";
    if (name == "setClipRect" || name == "clearClipRect") return "clip";
    return name;
}

// The Lua value at `index` as JSON: numbers, strings, booleans, and tables (a
// list: an array; with keys: an object, as drawString's options).
cJSON* to_json_value(lua_State* L, int index) {
    switch (lua_type(L, index)) {
        case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, index));
        case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, index));
        case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, index));
        case LUA_TTABLE: {
            index = lua_absindex(L, index);
            const lua_Unsigned n = lua_rawlen(L, index);
            if (n > 0) {
                cJSON* list = cJSON_CreateArray();
                for (lua_Unsigned i = 1; i <= n && i <= 8192; ++i) {
                    lua_rawgeti(L, index, static_cast<lua_Integer>(i));
                    cJSON_AddItemToArray(list, to_json_value(L, -1));
                    lua_pop(L, 1);
                }
                return list;
            }
            cJSON* object = cJSON_CreateObject();
            lua_pushnil(L);
            while (lua_next(L, index)) {
                if (lua_type(L, -2) == LUA_TSTRING) cJSON_AddItemToObject(object, lua_tostring(L, -2), to_json_value(L, -1));
                lua_pop(L, 1);
            }
            return object;
        }
        default: return cJSON_CreateNull();
    }
}

// The call's arguments (trailing nils dropped) as a JSON array.
cJSON* arguments(lua_State* L, int first) {
    int last = lua_gettop(L);
    while (last >= first && lua_isnil(L, last)) --last;
    cJSON* list = cJSON_CreateArray();
    for (int i = first; i <= last; ++i) cJSON_AddItemToArray(list, to_json_value(L, i));
    return list;
}

int lua_op(lua_State* L) {  // gfx.<function>(args…): one op into this frame
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    Frame& frame = frame_of(L);
    if (frame.ops.size() >= ui_canvas::kMaxOps) {
        return luaL_error(L, "一帧最多 %d 条指令：先 gfx.present()", static_cast<int>(ui_canvas::kMaxOps));
    }
    cJSON* item = arguments(L, 1);
    cJSON_InsertItemInArray(item, 0, cJSON_CreateString(name));
    Op op;
    String error;
    const bool ok = ui_canvas::parse_one(item, op, frame.images, frame.bytes, error);
    cJSON_Delete(item);
    if (!ok) return luaL_error(L, "gfx.%s：%s", name, error.c_str());
    if (op.kind == ui_canvas::Kind::DrawImage) return luaL_error(L, "gfx.drawImage 只在画布里用；程序里用 gfx.drawPng / drawJpg / drawBmp");
    const gfx_api::Fn* fn = op.kind == ui_canvas::Kind::Call ? &gfx_api::table()[op.fn] : nullptr;
    const bool state = (fn && fn->group == gfx_api::Group::State) || op.kind == ui_canvas::Kind::Rotate;
    const bool moves_cursor = fn && (!strcmp(fn->name, "print") || !strcmp(fn->name, "println"));
    if (state || moves_cursor) {  // the ruler follows, so queries see this
        if (Gfx* r = ruler(frame)) gfx_exec::run(*r, {op}, {});
    }
    // M5GFX: a drawing call given its optional colour sets the current colour
    // (fillRect(…, c) is setColor(c) then fillRect(…)), which later calls without
    // one use; so it is state too, kept for the next frames as a setColor.
    Op implied;
    if (fn && fn->group == gfx_api::Group::Draw && strstr(fn->params, "[c") &&
        op.args.size() == gfx_api::overload_params(*fn, op.overload).length()) {
        implied.fn = static_cast<int16_t>(gfx_api::index_of(gfx_api::find("setColor")));
        implied.args = {op.args.back()};
    }
    const auto keep = [&frame](const String& slot, const Op& value) {
        for (auto& s : frame.state) {
            if (s.first == slot) {
                s.second = value;
                return;
            }
        }
        frame.state.push_back({slot, value});
    };
    if (implied.fn >= 0) keep("setColor", implied);
    if (state) {
        keep(state_slot(name), op);
    }
    frame.ops.push_back(std::move(op));
    return 0;
}

int lua_query(lua_State* L) {  // gfx.width() … gfx.color565(r, g, b): answered at once
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    const gfx_api::Fn* fn = gfx_api::find(name);
    cJSON* args = arguments(L, 1);
    std::vector<String> v;
    int overload = 0;
    std::size_t bytes = 0;
    String error;
    const bool ok = gfx_api::check(*fn, args, v, overload, bytes, error);
    cJSON_Delete(args);
    if (!ok) return luaL_error(L, "gfx.%s：%s", name, error.c_str());
    const String n(name);
    const auto byte = [&](int k) { return static_cast<unsigned>(std::max(0, std::min(255, static_cast<int>(v[k].toInt())))); };
    if (n == "width") return lua_pushinteger(L, ui::kWidth), 1;
    if (n == "height") return lua_pushinteger(L, ui::kHeight), 1;
    if (n == "color565") return lua_pushinteger(L, gfx_api::rgb565(byte(0) << 16 | byte(1) << 8 | byte(2))), 1;
    if (n == "color332") return lua_pushinteger(L, (byte(0) & 0xE0) | ((byte(1) & 0xE0) >> 3) | (byte(2) >> 6)), 1;
    if (n == "color888") {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X", byte(0), byte(1), byte(2));
        return lua_pushstring(L, hex), 1;
    }
    Gfx* r = ruler(frame_of(L));
    if (!r) return luaL_error(L, "gfx.%s：内存不够，量不了", name);
    // A font named in the call for this query only; the program's current one otherwise.
    const auto with_font = [&](std::size_t k, auto measure) {
        if (k >= v.size()) return measure();
        const auto* saved = r->getFont();
        Op set;
        set.fn = static_cast<int16_t>(gfx_api::index_of(gfx_api::find("setFont")));
        set.args = {v[k]};
        gfx_exec::run(*r, {set}, {});
        const auto result = measure();
        r->setFont(saved);
        return result;
    };
    if (n == "textWidth") return lua_pushinteger(L, with_font(1, [&] { return r->textWidth(v[0].c_str()); })), 1;
    if (n == "textLength") return lua_pushinteger(L, r->textLength(v[0].c_str(), v[1].toInt())), 1;
    if (n == "fontHeight") return lua_pushinteger(L, with_font(0, [&] { return r->fontHeight(); })), 1;
    if (n == "fontWidth") return lua_pushinteger(L, with_font(0, [&] { return r->fontWidth(); })), 1;
    if (n == "getCursorX") return lua_pushinteger(L, r->getCursorX()), 1;
    if (n == "getCursorY") return lua_pushinteger(L, r->getCursorY()), 1;
    return luaL_error(L, "gfx.%s：没有这个查询", name);
}

int lua_present(lua_State* L) {  // gfx.present(): this frame to the screen, all at once
    const String refused = frame_policy();
    if (!refused.isEmpty()) return luaL_error(L, "gfx.present 被拒绝：%s", refused.c_str());
    // The UI takes a frame between two of its loops: wait for the last one to be
    // taken, so a program drawing faster than the screen does not lose frames.
    const unsigned long started = millis();
    while (ui::overlay_waiting() && millis() - started < kPaceMs) delay(2);
    Frame& frame = frame_of(L);
    std::vector<Op> ops = frame.prefix;  // the state the last frame ended with
    ops.insert(ops.end(), std::make_move_iterator(frame.ops.begin()), std::make_move_iterator(frame.ops.end()));
    const String error = show_frame(std::move(ops), !frame.shown);
    frame.shown = true;
    frame.ops.clear();
    frame.images = 0;
    frame.bytes = 0;
    // The next frame starts where this one ended: its state, and the cursor.
    frame.prefix.clear();
    for (const auto& s : frame.state) frame.prefix.push_back(s.second);
    if (frame.ruler) {
        Op cursor;
        cursor.fn = static_cast<int16_t>(gfx_api::index_of(gfx_api::find("setCursor")));
        cursor.args = {String(frame.ruler->getCursorX()), String(frame.ruler->getCursorY())};
        frame.prefix.push_back(cursor);
    }
    if (!error.isEmpty()) return luaL_error(L, "gfx.present：%s", error.c_str());
    return 0;
}

int lua_size(lua_State* L) {
    lua_pushinteger(L, ui::kWidth);
    lua_pushinteger(L, ui::kHeight);
    return 2;
}

int lua_measure(lua_State* L) {
    lua_pushinteger(L, ui::measure_text(luaL_checkstring(L, 1), static_cast<int>(luaL_optinteger(L, 2, 1))));
    return 1;
}

int lua_close(lua_State* L) {
    if (ui::open_panel() == kFrameName) ui::post_overlay(Overlay());
    frame_of(L).ops.clear();
    return 0;
}

// M5GFX's TFT_ colours (RGB565).
struct Constant {
    const char* name;
    uint16_t value;
};
const Constant kColors[] = {
    {"TFT_BLACK", 0x0000},   {"TFT_NAVY", 0x000F},        {"TFT_DARKGREEN", 0x03E0}, {"TFT_DARKCYAN", 0x03EF},
    {"TFT_MAROON", 0x7800},  {"TFT_PURPLE", 0x780F},      {"TFT_OLIVE", 0x7BE0},     {"TFT_LIGHTGREY", 0xD69A},
    {"TFT_LIGHTGRAY", 0xD69A}, {"TFT_DARKGREY", 0x7BEF},  {"TFT_DARKGRAY", 0x7BEF},  {"TFT_BLUE", 0x001F},
    {"TFT_GREEN", 0x07E0},   {"TFT_CYAN", 0x07FF},        {"TFT_RED", 0xF800},       {"TFT_MAGENTA", 0xF81F},
    {"TFT_YELLOW", 0xFFE0},  {"TFT_WHITE", 0xFFFF},       {"TFT_ORANGE", 0xFDA0},    {"TFT_GREENYELLOW", 0xB7E0},
    {"TFT_PINK", 0xFE19},    {"TFT_BROWN", 0x9A60},       {"TFT_GOLD", 0xFEA0},      {"TFT_SILVER", 0xC618},
    {"TFT_SKYBLUE", 0x867D}, {"TFT_VIOLET", 0x915C},
};

// FeiyuOS's own ops that programs may use too (drawImage is a canvas's).
const char* const kExtras[] = {"progressBar", "rotate", "drawQr"};

// gfx.<name>: a gfx_api function, one of FeiyuOS's own or a TFT_ colour; made
// once and kept in the table.
int lua_index(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    for (const auto& c : kColors) {
        if (!strcmp(c.name, name)) {
            lua_pushinteger(L, c.value);
            return 1;
        }
    }
    const gfx_api::Fn* fn = gfx_api::find(name);
    bool extra = false;
    for (const char* e : kExtras) extra = extra || !strcmp(e, name);
    if (!fn && !extra) return 0;  // nil, as for any missing field
    lua_pushstring(L, name);
    lua_pushcclosure(L, fn && fn->group == gfx_api::Group::Query ? lua_query : lua_op, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, 1);  // next time without the lookup
    return 1;
}

class GfxPackage : public Package {
public:
    GfxPackage() {
        ui_canvas::set_compiler([](const String& source) { return luavm::compile(source, "update"); });
    }

    const char* usage() const override { return kUsage; }
    const char* index() const override { return "画图 write gfx://canvas/名字（JSON：ops 绘制指令，可加 animate、vars 与 update、source），关闭 /close；程序里逐帧画 -- @uses gfx 后 gfx.fillRect(…) 等再 gfx.present()；read gfx://screen 看尺寸和配色；函数清单 read gfx://help"; }

    const Manifest& manifest() const override {
        static const Manifest m{"gfx", "画图：在屏幕上画图形、文字、图表、二维码、图片，画动画；程序里可以逐帧画",
                                "画 画图 绘制 图形 图表 进度条 动画 游戏 画面 屏幕 canvas draw gfx M5GFX", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("gfx://");
    }

    bool reversible(const String&, const cJSON*) const override { return true; }  // the screen

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String rest = url_decode(target_path(target).substring(strlen("gfx://")));
        const String content = json_str(arguments, "content");
        if (rest == "help") return gfx_api::help();
        if (rest == "screen") return kScreen;
        if (rest == "measure") {
            const String text = target_param(target, "text");
            const int size = target_param(target, "size").isEmpty() ? 1 : target_param(target, "size").toInt();
            return "「" + text + "」宽 " + String(ui::measure_text(text, size)) + " 像素（size " + String(size) + "）";
        }
        if (rest == "frame" || rest == "frame/close") {
            if (tool != "write") return "ERROR: gfx://frame 用 write（content 为一帧的指令数组）";
            if (rest == "frame/close") {
                if (ui::open_panel() == kFrameName) ui::post_overlay(Overlay());
                return "已收起画面";
            }
            std::vector<Op> ops;
            String error;
            if (!ui_canvas::parse_frame(content, ops, error)) return "ERROR: " + error;
            for (const auto& op : ops) {
                if (op.kind == ui_canvas::Kind::DrawImage) return "ERROR: 一帧不用 drawImage（用 drawPng / drawJpg / drawBmp，或画布 gfx://canvas/名字）";
            }
            const int count = ops.size();
            const String problem = show_frame(std::move(ops), true);
            if (!problem.isEmpty()) return "ERROR: " + problem;
            return "已显示一帧：" + String(count) + " 条绘制指令";
        }
        if (!rest.startsWith("canvas/")) {
            return "ERROR: 用 gfx://canvas/名字、gfx://frame、gfx://help、gfx://screen 或 gfx://measure?text=文字";
        }
        String name = rest.substring(7);
        String action;
        const int slash = name.indexOf('/');
        if (slash >= 0) {
            action = name.substring(slash + 1);
            name = name.substring(0, slash);
        }
        if (!ui_panel::valid_name(name)) return "ERROR: 名字不能为空、不超过 48 字节，不能含 / ? # & { } 和引号";
        if (action == "close") {
            if (ui::open_panel() == name) ui::post_overlay(Overlay());
            return "已关闭画布「" + name + "」";
        }
        if (tool == "read") return ui::open_panel() == name ? "画布「" + name + "」正在显示" : "画布「" + name + "」没有显示";
        if (!action.isEmpty()) return "ERROR: 画布只有 gfx://canvas/名字 与 /close";
        ui_canvas::Spec spec;
        String error;
        if (!ui_canvas::parse(content, spec, error)) return "ERROR: " + error;
        ui_live::show_canvas(name, spec);
        return "已显示画布「" + name + "」：" + String(static_cast<int>(spec.ops.size())) + " 条绘制指令" +
               (spec.source_tool.isEmpty() ? String() : "，打开期间按 source 刷新（{1} {2} 为结果各行）");
    }

    bool open_lua(lua_State* L) const override {
        Frame** box = static_cast<Frame**>(lua_newuserdatauv(L, sizeof(Frame*), 0));
        *box = new Frame();
        if (luaL_newmetatable(L, kFrameKey)) {
            lua_pushcfunction(L, frame_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        lua_setfield(L, LUA_REGISTRYINDEX, kFrameKey);
        // gfx.<name> is made the first time it is used (a program uses a few of
        // the ~80): the table starts with present, size, measure, close.
        lua_newtable(L);
        static const luaL_Reg kFunctions[] = {{"present", lua_present}, {"size", lua_size},
                                              {"measure", lua_measure}, {"close", lua_close},
                                              {nullptr, nullptr}};
        luaL_setfuncs(L, kFunctions, 0);
        lua_newtable(L);  // its metatable
        lua_pushcfunction(L, lua_index);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_setglobal(L, "gfx");
        // printf as on M5GFX: format, then print.
        luaL_dostring(L, "gfx.printf = function(fmt, ...) gfx.print(string.format(fmt, ...)) end");
        return true;
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"画图", "", "画一个每秒走动的时钟"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"gfx://help", "gfx：read 全部函数（同 M5GFX）、参数与例子"},
            {"gfx://canvas/名字", "gfx：write 画图（content 为 JSON：ops 指令数组 [函数名, 参数…]；可加 animate、vars、update、source）"},
            {"gfx://canvas/名字/close", "gfx：write 关闭画布"},
            {"gfx://frame", "gfx：write 显示一帧（content 为指令数组）；程序里 -- @uses gfx 后用 gfx.* 与 gfx.present()"},
            {"gfx://screen", "gfx：read 屏幕尺寸与主题色"},
            {"gfx://measure?text=文字&size=1", "gfx：read 文字宽度（像素）"},
        };
        for (const auto& item : items) add_resource(list, remaining, item[0], item[1]);
    }
};

}  // namespace

Package* make_gfx_package() { return new GfxPackage(); }

}  // namespace vat::registry
