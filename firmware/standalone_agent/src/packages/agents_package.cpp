// agents: Codex / Claude / Pi running read-only inside remote tmux.
// session(start_agent|resume_agent|interrupt|approve|reject).
#include "../common.h"
#include "../config.h"
#include "../ops_tui.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "语音中的 Voice Agent Terminal 项目名规范为 voice-agent-terminal；语音识别得到的英文项目名规范为小写，Backend 必须是 backend；已明确拼写的项目 slug 必须保持小写与连字符。用户让 Codex、Claude 或 Pi 在某项目完成任务时，session.action 使用 start_agent，并填 provider、project 和原始任务 prompt。如果启动 Agent 时没有项目名，必须直接澄清，不调用任何工具；“项目”“这个项目”“代码”等泛称不是项目名。恢复、中断、批准当前 Agent 分别使用 session 的 resume_agent、interrupt、approve，target 都是 agent://current。当前远程 Agent 使用 agent://current。结合语音场景纠正常见同音词，例如扣的差通常指 Codex。";

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
