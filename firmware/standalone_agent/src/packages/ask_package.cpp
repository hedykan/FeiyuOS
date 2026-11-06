// ask: put a question to the agent from a step that is not itself a
// conversation (a program run) and get the reply back as the result.
//   write ask://   content: the question
// The conversation is unattended: calls needing confirmation or a secret are
// refused, so it never waits for someone.  Refused while a conversation is
// already open (running or waiting for a confirmation), which also keeps the
// agent from asking itself.
#include "../agent.h"
#include "../common.h"
#include "../registry.h"
#include "../ui_state.h"

namespace vat::registry {
namespace {

class AskPackage : public Package {
public:
    const Manifest& manifest() const override {
        static const Manifest m{"ask", "在程序里问大肥鱼一句（ask://），回复作为这一步的结果；只在程序中使用",
                                "ask 问 判断 大肥鱼 程序", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "write" || tool == "read") && json_str(arguments, "target").startsWith("ask://");
    }

    // Asking is read-only for the device; what the answer leads to is the
    // program's own (confirmed) steps.
    bool reversible(const String&, const cJSON*) const override { return true; }

    String execute(const String&, const cJSON* arguments, bool) override {
        String question = json_str(arguments, "content");
        if (question.isEmpty()) question = url_decode(json_str(arguments, "target").substring(strlen("ask://")));
        question.trim();
        if (question.isEmpty()) return "ERROR: ask:// 要在 content 里写问题";
        if (agent::active()) return "ERROR: 已有对话在进行或等待确认，ask:// 只能在程序里使用";
        const auto outcome = agent::run(question, [](const String& stage) { ui::set_stage("ASK " + stage); }, true);
        if (!outcome.ok) return "ERROR: " + outcome.error;
        return outcome.reply;
    }
};

}  // namespace

Package* make_ask_package() { return new AskPackage(); }

}  // namespace vat::registry
