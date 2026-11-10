// ui: panels the agent writes on the fly, and local apps (saved panels).
//   write ui://panel/<名字>          content: panel JSON (ui_panel.h); shows it
//   read  ui://panel/<名字>          the last choice made on it
//   write ui://panel/<名字>/close
//   write ui://apps/<名字>           content: panel JSON; saves it as an app
//   write ui://apps/<名字>/open | /delete
//   read  ui://apps                  list
// Choosing on a panel emits ui://panel/<名字> ({event.choice}, {event.index}),
// usable as a trigger.  A panel's source is re-run while it stays open, only
// when Policy allows the call without asking.  Fn+A (an event from the core)
// opens the app list.
#include <mutex>

#include "../common.h"
#include "../events.h"
#include "../policy.h"
#include "../registry.h"
#include "../storage.h"
#include "../lua_vm.h"
#include "../ui_canvas.h"
#include "../ui_panel.h"
#include "../ui_state.h"
#include "../worker.h"

namespace vat::registry {
namespace {

using ui::open_panel;
using ui::Overlay;
using ui::panel_choice;
using ui::post_overlay;

constexpr char kAppDir[] = "/vat/things/apps";

// The panel on screen with a source: refreshed from the worker while open.
std::mutex lock;
String live_name;
ui_panel::Spec live;
unsigned long next_refresh = 0;
bool refresh_queued = false;
bool live_seen = false;  // the live panel has been on screen
bool live_is_canvas = false;
ui_canvas::Spec live_canvas;

String app_path(const String& name) { return String(kAppDir) + "/" + name + ".json"; }

// A canvas with its source lines filled in; the drawImage file is read here
// (worker) so the UI task only draws.
void post_canvas(const String& name, const ui_canvas::Spec& spec, const std::vector<String>& lines) {
    Overlay overlay;
    overlay.kind = Overlay::Kind::Canvas;
    overlay.panel = name;
    overlay.canvas = spec.ops;
    overlay.frame_ms = spec.frame_ms;
    overlay.frames = spec.frames;
    overlay.step = spec.step;
    overlay.vars_json = spec.vars_json;
    overlay.update = spec.update;
    ui_canvas::fill(overlay.canvas, lines);
    for (const auto& op : overlay.canvas) {
        if (op.kind != ui_canvas::Kind::DrawImage) continue;
        ui_canvas::number(op.args[1], overlay.image_x);
        ui_canvas::number(op.args[2], overlay.image_y);
        if (!storage::read_bytes(op.args[0], overlay.image, 32768)) overlay.image.clear();
    }
    post_overlay(std::move(overlay));
}

void show_canvas(const String& name, const ui_canvas::Spec& spec) {
    post_canvas(name, spec, {});
    std::lock_guard<std::mutex> guard(lock);
    live_name = spec.source_tool.isEmpty() ? String() : name;
    live_is_canvas = true;
    live_canvas = spec;
    live_seen = false;
    next_refresh = millis();
}

void show(const String& name, const ui_panel::Spec& spec) {
    Overlay overlay;
    overlay.kind = Overlay::Kind::Panel;
    overlay.panel = name;
    overlay.title = spec.title.isEmpty() ? name : spec.title;
    overlay.lines = spec.lines;
    overlay.choices = spec.choices;
    post_overlay(std::move(overlay));
    std::lock_guard<std::mutex> guard(lock);
    live_name = spec.source_tool.isEmpty() ? String() : name;
    live = spec;
    live_is_canvas = false;
    live_seen = false;
    next_refresh = millis();  // the first refresh right away
}

// Worker: run the live panel's source and show the result.
void refresh_job(uint32_t id, const String& name) {
    ui_panel::Spec spec;
    bool canvas;
    ui_canvas::Spec canvas_spec;
    {
        std::lock_guard<std::mutex> guard(lock);
        refresh_queued = false;
        if (live_name != name) return;
        spec = live;
        canvas = live_is_canvas;
        canvas_spec = live_canvas;
        if (canvas) {
            spec.source_tool = canvas_spec.source_tool;
            spec.source_args = canvas_spec.source_args;
        }
    }
    if (open_panel() != name) return;
    cJSON* args = cJSON_Parse(spec.source_args.c_str());
    String text;
    if (!args) {
        text = "刷新来源格式不对";
    } else {
        if (!cJSON_GetObjectItemCaseSensitive(args, "confirm_required")) cJSON_AddBoolToObject(args, "confirm_required", false);
        const auto verdict = policy::evaluate(spec.source_tool, args);
        text = verdict.decision == policy::Decision::Allow ? registry::execute(spec.source_tool, args, false)
                                                         : "这个来源需要确认，不能自动刷新（" + verdict.reason + "）";
        cJSON_Delete(args);
    }
    worker::set_result(id, "refreshed " + name);
    if (open_panel() != name) return;
    if (canvas) return post_canvas(name, canvas_spec, ui_panel::lines_from(text));
    spec.lines = ui_panel::lines_from(text);
    Overlay overlay;
    overlay.kind = Overlay::Kind::Panel;
    overlay.panel = name;
    overlay.title = spec.title.isEmpty() ? name : spec.title;
    overlay.lines = spec.lines;
    overlay.choices = spec.choices;
    post_overlay(std::move(overlay));
}

std::vector<String> app_names() {
    std::vector<String> names;
    cJSON* list = cJSON_Parse(storage::list_dir(kAppDir).c_str());
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        const String file = json_str(entry, "name");
        if (file.endsWith(".json")) names.push_back(file.substring(0, file.length() - 5));
    }
    cJSON_Delete(list);
    return names;
}

String open_app(const String& name) {
    ui_panel::Spec spec;
    String error;
    if (!storage::exists(app_path(name))) return "ERROR: 没有应用「" + name + "」（read ui://apps 查看）";
    const String json = storage::read_text(app_path(name), ui_canvas::kMaxJson + 64);
    if (json.indexOf("\"ops\"") >= 0) {
        ui_canvas::Spec canvas;
        if (!ui_canvas::parse(json, canvas, error)) return "ERROR: 应用「" + name + "」的描述坏了：" + error;
        show_canvas(name, canvas);
        return "已打开应用「" + name + "」";
    }
    if (!ui_panel::parse(json, spec, error)) {
        return "ERROR: 应用「" + name + "」的描述坏了：" + error;
    }
    show(name, spec);
    return "已打开应用「" + name + "」";
}

constexpr char kUsage[] =
    "即时界面（ui://）：要给用户看一个小面板、或让用户在几个选项里选一个时用。write target=ui://panel/名字，content 为 JSON 对象："
    "title 标题；lines 文字行数组（最多 12 行）；choices 选项数组（最多 8 个）；source 可选，打开期间定时刷新 lines，"
    "写成 read 或 exec 加目标，或 tool 加 arguments（与工具调用同形），再加 every（如 30s，至少 10 秒）。"
    "用户选定后 read ui://panel/名字 取结果，或建触发监听 ui://panel/名字（{event.choice}）。"
    "存成可反复打开的本地应用：write ui://apps/名字（content 同上）；打开 write ui://apps/名字/open；列出 read ui://apps；"
    "删除 write ui://apps/名字/delete。设备上 Fn+A 打开程序页，程序和本地应用都列在那里。"
    "自由绘图用 write target=ui://canvas/名字，content 为 JSON 对象，ops 为绘制指令数组，每条写成 [指令名, 参数…]，"
    "指令与 M5GFX 同名同参：fillScreen、drawPixel、drawLine、drawFastHLine、drawFastVLine、drawRect、fillRect、"
    "drawRoundRect、fillRoundRect、drawCircle、fillCircle、drawEllipse、fillEllipse、drawTriangle、fillTriangle、"
    "drawArc、fillArc（x y 内半径 外半径 起角 止角）、drawBezier、drawGradientLine（两色）、drawString（文字 x y，"
    "可加选项 size 1 到 4、color、datum 如 middle_center）、drawQr（文字 x y 宽）、drawImage（/vat/ 下图片 x y，最多一张）、"
    "progressBar（x y 宽 高 百分比 颜色 底色），颜色写 #RRGGBB 或 white red cyan 等；屏幕 240×135，最多 64 条；"
    "旋转用 [rotate, 角度, 中心x, 中心y] 作用于其后的图形，[rotate, 0, 0, 0] 取消；"
    "动画在 content 顶层加 animate：every 每帧间隔如 100ms（至少 33ms）、frames 帧数（0 表示一直到关闭）、step 每帧增量，"
    "参数里的 {t} 换成 帧序号乘 step（{t} {1} {1#} 这类占位符在 JSON 里写成带引号的字符串），例如椭圆原地旋转：ops 先 [rotate, {t}, 120, 67] 再 [fillEllipse, 120, 67, 50, 20, cyan]，"
    "animate 为 every 100ms、frames 0、step 10。"
    "变量动画：顶层加 vars（初值对象，如 x 为 20、vx 为 6）和 update（每帧先执行的一段 Lua 代码，如 x = x + vx  if x > 220 or x < 20 then vx = -vx end；可用 vars 里的变量、t 帧序号乘 step、math，以及 vat.stream 读传感器，不能调用其他能力），参数里写 {x} 取变量值；有 update 没 animate 时每 33ms 一帧直到关闭。"
    "可加 source 定时刷新，参数里的 {1} {2} 换成结果的第 1、2 行，{1#} 换成第 1 行里的第一个数字（用于大数字和进度条）。画布也能存成应用（ui://apps/名字，content 同上）。";

class UiPackage : public Package {
public:
    UiPackage() {
        ui_canvas::set_compiler([](const String& source) { return luavm::compile(source, "update"); });
    }

    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"ui", "即时界面：在屏幕上画面板（文字、选项、定时刷新）和自由绘图画布（线、形状、文字、图表、图片），保存为可反复打开的本地应用",
                                "界面 面板 选项 选择 菜单 应用 小程序 仪表盘 画 绘制 画图 图表 进度条 UI panel canvas app", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("ui://");
    }

    bool reversible(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return true;
        return !url_decode(json_str(arguments, "target")).endsWith("/delete");
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        const String rest = url_decode(json_str(arguments, "target").substring(5));
        if (tool == "write" && rest.startsWith("apps/") && rest.endsWith("/delete")) {
            return "删除本地应用「" + rest.substring(5, rest.length() - 7) + "」";
        }
        return {};
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String rest = url_decode(json_str(arguments, "target").substring(strlen("ui://")));
        const String content = json_str(arguments, "content");
        if (rest == "apps" || rest == "apps/") {
            const auto names = app_names();
            if (names.empty()) return "还没有本地应用";
            String out = String(static_cast<int>(names.size())) + " 个本地应用：";
            for (std::size_t i = 0; i < names.size(); ++i) out += (i ? "、" : "") + names[i];
            return out;
        }
        const bool canvas = rest.startsWith("canvas/");
        const bool panel = rest.startsWith("panel/");
        const bool app = rest.startsWith("apps/");
        if (!panel && !app && !canvas) return "ERROR: 用 ui://panel/名字、ui://canvas/名字 或 ui://apps/名字";
        String name = rest.substring(canvas ? 7 : panel ? 6 : 5);
        String action;
        const int slash = name.indexOf('/');
        if (slash >= 0) {
            action = name.substring(slash + 1);
            name = name.substring(0, slash);
        }
        if (!ui_panel::valid_name(name)) return "ERROR: 名字不能为空、不超过 48 字节，不能含 / ? # & { } 和引号";
        if (canvas) {
            if (action == "close") {
                if (open_panel() == name) post_overlay(Overlay());
                return "已关闭画布「" + name + "」";
            }
            if (tool == "read") return open_panel() == name ? "画布「" + name + "」正在显示" : "画布「" + name + "」没有显示";
            if (!action.isEmpty()) return "ERROR: 画布只有 ui://canvas/名字 与 /close";
            ui_canvas::Spec spec;
            String error;
            if (!ui_canvas::parse(content, spec, error)) return "ERROR: " + error;
            show_canvas(name, spec);
            return "已显示画布「" + name + "」：" + String(static_cast<int>(spec.ops.size())) + " 条绘制指令" +
                   (spec.source_tool.isEmpty() ? String() : "，打开期间按 source 刷新（{1} {2} 为结果各行）");
        }
        if (panel) {
            if (tool == "read") {
                const String choice = panel_choice(name);
                return choice.isEmpty() ? "面板「" + name + "」上还没有选择" : "面板「" + name + "」上选了：" + choice;
            }
            if (action == "close") {
                if (open_panel() == name) post_overlay(Overlay());
                return "已关闭面板「" + name + "」";
            }
            if (!action.isEmpty()) return "ERROR: 面板只有 ui://panel/名字 与 /close";
            ui_panel::Spec spec;
            String error;
            if (!ui_panel::parse(content, spec, error)) return "ERROR: " + error;
            show(name, spec);
            return "已显示面板「" + name + "」" + (spec.choices.empty() ? String() : "，等用户选择（read ui://panel/" + name + " 取结果）") +
                   (spec.source_tool.isEmpty() ? String() : "，打开期间按 source 刷新");
        }
        if (tool == "read" || action == "open") return open_app(name);
        if (action == "delete") {
            if (!storage::exists(app_path(name))) return "ERROR: 没有应用「" + name + "」";
            storage::remove(app_path(name));
            return "已删除应用「" + name + "」";
        }
        if (!action.isEmpty()) return "ERROR: 应用用 ui://apps/名字（保存）、/open 或 /delete";
        String error, saved;
        if (content.indexOf("\"ops\"") >= 0) {  // a canvas, kept as written once it checks out
            ui_canvas::Spec canvas;
            if (!ui_canvas::parse(content, canvas, error)) return "ERROR: " + error;
            saved = content;
        } else {
            ui_panel::Spec spec;
            if (!ui_panel::parse(content, spec, error)) return "ERROR: " + error;
            saved = ui_panel::to_json(spec);
        }
        const bool existed = storage::exists(app_path(name));
        if (!storage::write_text(app_path(name), saved)) return "ERROR: 保存失败";
        return String(existed ? "已更新" : "已保存") + "应用「" + name + "」，Fn+A 或 write ui://apps/" + name + "/open 打开";
    }

    void triggers(std::vector<TriggerSpec>& out) const override {
        out.push_back({"ui://panel/面板名", "用户在这个面板上选定一项时（{event.choice} 选项、{event.index} 第几个）"});
    }

    bool watch(const String& target, String& normalized, String&) override {
        if (!target.startsWith("ui://panel/")) return false;
        normalized = target;  // emitted on every choice anyway
        return true;
    }

    void loop() override {
        String name;
        uint32_t every_s;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (live_name.isEmpty() || refresh_queued || static_cast<long>(millis() - next_refresh) < 0) return;
            name = live_name;
            every_s = live_is_canvas ? live_canvas.every_s : live.every_s;
        }
        const bool on_screen = open_panel() == name;
        if (!on_screen) {
            // Not drawn yet (just posted): wait.  Seen and gone: closed or replaced, stop.
            std::lock_guard<std::mutex> guard(lock);
            if (live_seen && live_name == name) live_name = "";
            return;
        }
        {
            std::lock_guard<std::mutex> guard(lock);
            live_seen = true;
        }
        if (worker::busy() || !worker::submit({worker::Kind::Callback, name, "", {}, false, refresh_job})) return;
        std::lock_guard<std::mutex> guard(lock);
        refresh_queued = true;
        next_refresh = millis() + (every_s ? every_s * 1000UL : 0x7FFFFFFFUL);
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"面板", "", "做一个看开发机负载的小面板，每 30 秒刷新，存成应用"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"ui://panel/名字", "ui：write 显示面板（content 为 JSON：title、lines、choices、source），read 取用户的选择"},
            {"ui://panel/名字/close", "ui：write 关闭面板"},
            {"ui://canvas/名字", "ui：write 自由绘图（content 为 JSON：ops 绘制指令数组，与 M5GFX 同名同参；可加 source）"},
            {"ui://apps", "ui：read 列出本地应用"},
            {"ui://apps/名字", "ui：write 保存为本地应用（content 同面板）；/open 打开；/delete 删除"},
        };
        for (const auto& item : items) add_resource(list, remaining, item[0], item[1]);
    }
};

}  // namespace

Package* make_ui_package() { return new UiPackage(); }

}  // namespace vat::registry
