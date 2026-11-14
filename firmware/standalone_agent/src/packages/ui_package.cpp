// ui: panels the agent writes on the fly, and local apps (saved panels or
// canvases; drawing itself is the gfx package).
//   write ui://panel/<名字>          content: panel JSON (ui_panel.h); shows it
//   read  ui://panel/<名字>          the last choice made on it
//   write ui://panel/<名字>/close
//   write ui://apps/<名字>           content: panel JSON; saves it as an app
//   write ui://apps/<名字>/open | /delete
//   read  ui://apps/list             list
// Choosing on a panel emits ui://event/panel/<名字> ({event.choice}, {event.index}),
// usable as a trigger.  A panel's source is re-run while it stays open, only
// when Policy allows the call without asking.  Fn+A (an event from the core)
// opens the app list.
#include "../common.h"
#include "../events.h"
#include "../registry.h"
#include "../storage.h"
#include "../ui_canvas.h"
#include "../ui_live.h"
#include "../ui_panel.h"
#include "../ui_state.h"

namespace vat::registry {
namespace {

using ui::open_panel;
using ui::Overlay;
using ui::panel_choice;
using ui::post_overlay;

constexpr char kAppDir[] = "/vat/things/apps";

String app_path(const String& name) { return String(kAppDir) + "/" + name + ".json"; }

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
    if (!storage::exists(app_path(name))) return "ERROR: 没有应用「" + name + "」（read ui://apps/list 查看）";
    const String json = storage::read_text(app_path(name), ui_canvas::kMaxJson + 64);
    if (json.indexOf("\"ops\"") >= 0) {
        ui_canvas::Spec canvas;
        if (!ui_canvas::parse(json, canvas, error)) return "ERROR: 应用「" + name + "」的描述坏了：" + error;
        ui_live::show_canvas(name, canvas);
        return "已打开应用「" + name + "」";
    }
    if (!ui_panel::parse(json, spec, error)) {
        return "ERROR: 应用「" + name + "」的描述坏了：" + error;
    }
    ui_live::show_panel(name, spec);
    return "已打开应用「" + name + "」";
}

constexpr char kUsage[] =
    "即时界面（ui://）：要给用户看一个小面板、或让用户在几个选项里选一个时用。write target=ui://panel/名字，content 为 JSON 对象："
    "title 标题；lines 文字行数组（最多 12 行）；choices 选项数组（最多 8 个）；source 可选，打开期间定时刷新 lines，"
    "写成 read 或 exec 加目标，或 tool 加 arguments（与工具调用同形），再加 every（如 30s，至少 10 秒）。"
    "用户选定后 read ui://panel/名字 取结果，或建触发监听 ui://event/panel/名字（{event.choice}）。"
    "存成可反复打开的本地应用：write ui://apps/名字（content 同上）；打开 write ui://apps/名字/open；列出 read ui://apps/list；"
    "删除 write ui://apps/名字/delete。设备上 Fn+A 打开程序页，程序和本地应用都列在那里。"
    "画图（画布）见 gfx 包（gfx://canvas/名字），画布也能存成本地应用（ui://apps/名字，content 同画布）。";

class UiPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "write ui://panel/名字，本地应用 write ui://apps/名字，read ui://apps/list，选择事件 ui://event/panel/名字"; }

    const Manifest& manifest() const override {
        static const Manifest m{"ui", "即时界面：在屏幕上显示面板（文字、选项、定时刷新），把面板或画布保存为可反复打开的本地应用",
                                "界面 面板 选项 选择 菜单 应用 小程序 仪表盘 UI panel app", {}};
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
        if (rest == "apps/list") {
            const auto names = app_names();
            if (names.empty()) return "还没有本地应用";
            String out = String(static_cast<int>(names.size())) + " 个本地应用：";
            for (std::size_t i = 0; i < names.size(); ++i) out += (i ? "、" : "") + names[i];
            return out;
        }
        const bool panel = rest.startsWith("panel/");
        const bool app = rest.startsWith("apps/");
        if (!panel && !app) return "ERROR: 用 ui://panel/名字 或 ui://apps/名字（画布是 gfx://canvas/名字）";
        String name = rest.substring(panel ? 6 : 5);
        String action;
        const int slash = name.indexOf('/');
        if (slash >= 0) {
            action = name.substring(slash + 1);
            name = name.substring(0, slash);
        }
        if (!ui_panel::valid_name(name)) return "ERROR: 名字不能为空、不超过 48 字节，不能含 / ? # & { } 和引号";
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
            ui_live::show_panel(name, spec);
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
        if (name == "list") return "ERROR: 应用不能叫 list（ui://apps/list 是列出全部应用）";
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
        out.push_back({"ui://event/panel/面板名", "用户在这个面板上选定一项时（{event.choice} 选项、{event.index} 第几个）"});
    }

    bool watch(const String& target, String& normalized, String&) override {
        if (!target.startsWith("ui://event/panel/")) return false;
        normalized = target;  // emitted on every choice anyway
        return true;
    }

    void loop() override { ui_live::loop(); }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"面板", "", "做一个看开发机负载的小面板，每 30 秒刷新，存成应用"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"ui://panel/名字", "ui：write 显示面板（content 为 JSON：title、lines、choices、source），read 取用户的选择"},
            {"ui://panel/名字/close", "ui：write 关闭面板"},
            {"ui://apps/list", "ui：read 列出本地应用"},
            {"ui://apps/名字", "ui：write 保存为本地应用（content 同面板或画布）；/open 打开；/delete 删除"},
        };
        for (const auto& item : items) add_resource(list, remaining, item[0], item[1]);
    }
};

}  // namespace

Package* make_ui_package() { return new UiPackage(); }

}  // namespace vat::registry
