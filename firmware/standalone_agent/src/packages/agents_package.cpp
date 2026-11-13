// agents: Codex / Claude / Pi running read-only inside remote tmux.
// session(start_agent|resume_agent|interrupt|approve|reject).
#include "../common.h"
#include "../config.h"
#include "../ops_tui.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "远程 Agent（Codex、Claude、Pi）：让它在某项目做事用 session(start_agent, provider, project, prompt=用户的任务原话)；没有具体项目名（“这个项目”“代码”不算）就澄清。当前 Agent 是 agent://current：恢复 resume_agent、中断 interrupt、批准 approve、拒绝 reject。项目名用小写和连字符。";

class AgentsPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"agents", "Codex、Claude、Pi 远程 Agent", "agent codex claude pi 编程", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        if (tool != "session") return false;
        const String action = json_str(arguments, "action");
        return action == "start_agent" || action == "resume_agent" || action == "interrupt" ||
               action == "approve" || action == "reject";
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"编程助手", "", "让 Codex 看看 voice-agent-terminal 项目"});
    }

    String execute(const String&, const cJSON* arguments, bool) override {
        const String action = json_str(arguments, "action");
        const String target = json_str(arguments, "target");
        if (action == "start_agent") {
            String profile = profile_of(target);
            if (profile.isEmpty()) profile = config::get().default_profile;
            return tui::start_agent(json_str(arguments, "provider"), json_str(arguments, "project"),
                                    json_str(arguments, "prompt"), profile);
        }
        if (action == "resume_agent") {
            if (!tui::has_current()) return "当前没有可恢复的远程 Agent";
            return "已恢复 " + tui::current().target() + "\n" + tui::compact_screen(tui::observe("tui://current"));
        }
        if (action == "interrupt") return tui::interrupt(target.isEmpty() ? String("tui://current") : target);
        if (action == "approve") return tui::send_input("tui://current", "", {"enter"}, false) + "（批准）";
        return tui::send_input("tui://current", "", {"esc"}, false) + "（拒绝）";
    }
};

}  // namespace

Package* make_agents_package() { return new AgentsPackage(); }

}  // namespace vat::registry
