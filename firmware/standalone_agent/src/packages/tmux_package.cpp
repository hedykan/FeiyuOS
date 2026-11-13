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
constexpr char kUsage[] = "终端：当前终端、TUI、Codex、Claude 或“这里”都是 tui://current：看输出 observe(tui://current)，打字或按键 input(tui://current)。按键写 CTRL_C、UP、ENTER 这样的大写名；只按键或用户说不要发送时 submit=false，要执行或发送时 true。tmux：列会话 session(list, tmux://主机)，新建或连接 session(create 或 attach, tmux://主机/会话名)，主机不能省。终端在等密码时请用户到 SSH 页输入。";

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
