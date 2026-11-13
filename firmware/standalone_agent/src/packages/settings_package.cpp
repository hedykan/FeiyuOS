// settings: the settings page for the agent.  settings:// reads every row;
// a few plain choices can be written; everything typed (Keys, Wi-Fi
// passwords) only opens the page on the device, so no secret passes the
// model.  Work runs on the UI task (settings::on_ui).  brightness, timeout,
// volume and context are the device's own (local::tool_read / tool_write);
// settings://lora/… and settings://ble/power belong to those packages
// (registered before this one).
#include <algorithm>

#include "../common.h"
#include "../config.h"
#include "../ops_local.h"
#include "../registry.h"
#include "../settings_page.h"
#include "../settings_store.h"
#include "../ui_status.h"

namespace vat::registry {
namespace {

constexpr char kUsage[] =
    "设置（settings://）：read settings:// 看设置页各项当前值；write settings://brightness（8–255）、settings://timeout（秒，0 为永不）、"
    "settings://volume（0–100）、settings://context（短、中、长）；write settings://animation content 为 开、减弱 或 关；"
    "write settings://statusbar content 为顶栏要显示的项，按顺序用逗号分隔（时间、电量、信号、WiFi名、内存、蓝牙、后台失败，空表示都不显示）；"
    "write settings://font content 为终端字号 10、12 或 14；"
    "要输入 Key 时 write settings://key/qwen/open（或 deepseek、openrouter、amap）在设备上弹出输入框，由用户在键盘上输入，不要向用户索要 Key；"
    "write settings://页名/open 打开设置页（wifi keys hosts lora ble statusbar animation font context storage portal webtoken）。";

String trimmed(String text) {
    text.trim();
    return text;
}

String set_animation(const String& value) {
    int level = -1;
    if (value == "开" || value == "on" || value == "2") level = 2;
    else if (value == "减弱" || value == "reduced" || value == "1") level = 1;
    else if (value == "关" || value == "off" || value == "0") level = 0;
    if (level < 0) return "ERROR: 动画只能是 开、减弱 或 关";
    const String error = settings_store::set_animation(level);
    if (!error.isEmpty()) return "ERROR: " + error;
    return "动画已设为" + value;
}

String set_statusbar(String value) {
    value.replace("，", ",");
    value.replace("、", ",");
    value.replace("·", ",");
    std::vector<String> ids;
    String unknown;
    int from = 0;
    while (from <= static_cast<int>(value.length())) {
        int comma = value.indexOf(',', from);
        if (comma < 0) comma = value.length();
        const String part = trimmed(value.substring(from, comma));
        from = comma + 1;
        if (part.isEmpty()) continue;
        String id;
        for (const auto& item : ui::status::all()) {
            if (part == item.id || part == item.label || (part == "信号" && String(item.id) == "signal") ||
                (part == "WiFi名" && String(item.id) == "ssid") || (part == "内存" && String(item.id) == "heap")) {
                id = item.id;
            }
        }
        if (id.isEmpty()) unknown += (unknown.isEmpty() ? "" : "、") + part;
        else if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    if (!unknown.isEmpty()) {
        String names;
        for (const auto& item : ui::status::all()) names += (names.isEmpty() ? "" : "、") + String(item.label);
        return "ERROR: 顶栏没有这些项：" + unknown + "（可选：" + names + "）";
    }
    if (!ui::status::save(ids)) return "ERROR: 顶栏设置保存失败";
    return "顶栏已设为：" + ui::status::summary();
}

class SettingsPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override {
        return "read settings:// 看各项；write settings://brightness|timeout|volume|context|animation|statusbar|font；打开设置页 write settings://页名/open，Key 输入框 settings://key/提供方/open";
    }

    const Manifest& manifest() const override {
        static const Manifest m{"settings", "设置页：查看各项、调动画、顶栏、字号，在设备上打开 Key 输入",
                                "设置 动画 顶栏 状态栏 字号 Key 密钥 令牌 热点", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("settings://");
    }

    // Opening the hotspot or the web token page asks every time; the rest
    // only changes how the device looks, or opens a page for the user.
    bool reversible(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        return tool == "write" && target != "settings://portal/open" && target != "settings://webtoken/open" &&
               target != "settings://context";
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String path = target.substring(String("settings://").length());
        if (path == "brightness" || path == "timeout" || path == "volume" || path == "context") {
            return tool == "read" ? local::tool_read(arguments) : local::tool_write(arguments);
        }
        if (tool == "read") {
            if (!path.isEmpty()) return "ERROR: 能 read 的是 settings:// 和 settings://brightness、timeout、volume、context";
            return ui::settings::on_ui([] { return ui::settings::rows(); });
        }
        const String value = trimmed(json_str(arguments, "content"));
        if (path == "animation") return ui::settings::on_ui([value] { return set_animation(value); });
        if (path == "statusbar") return ui::settings::on_ui([value] { return set_statusbar(value); });
        if (path == "font") {
            return ui::settings::on_ui([value] {
                const String error = settings_store::set_terminal_font(value.toInt());
                return error.isEmpty() ? "终端字号已设为 " + value : "ERROR: " + error;
            });
        }
        if (path == "open" || path.endsWith("/open")) {
            const String page = path == "open" ? String() : path.substring(0, path.length() - 5);
            return ui::settings::on_ui([page] { return ui::settings::show_page(page); });
        }
        return "ERROR: 不认识 " + target + "（read settings://；write settings://brightness、timeout、volume、context、animation、statusbar、font，打开设置页 settings://页名/open）";
    }
};

}  // namespace

Package* make_settings_package() { return new SettingsPackage(); }

}  // namespace vat::registry
