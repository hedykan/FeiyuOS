// tmux: observe and type into remote tmux/TUI sessions.  Targets:
// tui://current, tui://PROFILE/NAME, agent://current (observe),
// tmux://PROFILE[/NAME] for session(list|create|attach|connect).
#include "../common.h"
#include "../ops_tui.h"
#include "../registry.h"

namespace vat::registry {
namespace {

bool tui_target(const String& target) {
    return target.isEmpty() || target.startsWith("tui://") || target.startsWith("agent://") ||
           target.startsWith("tmux://");
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "用户说当前终端、当前 TUI、当前 Codex、当前 Claude 或这里，并要求输入文字/按键时，input 的 target 一律使用 tui://current，绝不能使用 agent://current；agent://current 只用于 session 的恢复、中断、批准或拒绝。用户查看当前终端输出时用 observe(target=tui://current)。Control C 等按键使用 input.keys=[CTRL_C] 且 submit=false。上方向键使用 input.keys=[UP] 且 submit=false。tmux 会话使用 tmux://PROFILE/NAME；列出会话时 target 为 tmux://PROFILE，创建或连接命名会话时 target 为 tmux://PROFILE/NAME。语音识别可能把‘X 的 tmux 会话’写成 XTheTMUX绘画、XTMX绘画；此时 TMUX/TMX/绘画是 tmux/会话，不属于会话名，会话名只取 X 并规范成小写。用户明确说不要发送时 submit=false，否则明确要求执行或发送时为 true。观察到终端在等待密码时，告诉用户到 SSH 页输入，不要代输。";

class TmuxPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"tmux", "远端 TUI/tmux 的观察与输入", "tmux 终端 会话 输入 输出", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (tool == "observe" || tool == "input") return tui_target(target);
        if (tool == "read") return target.startsWith("tui://") || target.startsWith("agent://");
        if (tool != "session") return false;
        const String action = json_str(arguments, "action");
        if (action == "create") return true;
        if (action == "attach") return target.isEmpty() || tui_target(target);
        return (action == "list" || action == "connect") && target.startsWith("tmux://");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        if (tool == "observe" || tool == "read") return tui::tool_observe(arguments);
        if (tool == "input") return tui::tool_input(arguments);
        const String action = json_str(arguments, "action");
        const String target = json_str(arguments, "target");
        if (action == "list") return tui::tmux_list(target);
        if (action == "create") return tui::tmux_create(target);
        if (target.isEmpty() || target == "agent://current") {
            return tui::has_current() ? "当前 TUI：" + tui::current().target() : String("当前没有活动 TUI");
        }
        return tui::tmux_attach(target);
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"终端", tui::has_current() ? tui::current().name : String(), "在终端输入 ls 并回车"});
    }

    void resources(cJSON* list, int& remaining) override {
        if (!tui::has_current()) return;
        add_resource(list, remaining, tui::current().target().c_str(), "tui_session");
    }
};

}  // namespace

Package* make_tmux_package() { return new TmuxPackage(); }

}  // namespace vat::registry
