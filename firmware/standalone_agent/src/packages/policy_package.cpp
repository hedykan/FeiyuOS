// policy: session(set_policy) changes the per-target deny/ask/allow/allow_all
// mode.  The change itself always requires confirmation (see policy.cpp).
#include "../common.h"
#include "../config.h"
#include "../ops_tui.h"
#include "../policy.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "权限策略 deny/ask/allow/allow_all：改策略用 session(set_policy, target, policy_mode)，要确认。";

class PolicyPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"policy", "目标权限策略 deny/ask/allow/allow_all", "权限 策略 确认 policy", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "session" && json_str(arguments, "action") == "set_policy";
    }

    void hints(std::vector<Hint>& out) override {
        const String host = tui::has_current() ? tui::current().profile : config::get().default_profile;
        out.push_back({"权限", policy::mode_for("ssh://" + host), host + "改成每次确认"});
    }

    String execute(const String&, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String mode = json_str(arguments, "policy_mode");
        if (mode != "deny" && mode != "ask" && mode != "allow" && mode != "allow_all") return "无效策略: " + mode;
        policy::set_mode(target, mode);
        return "已将 " + target + " 的策略设为 " + mode;
    }
};

}  // namespace

Package* make_policy_package() { return new PolicyPackage(); }

}  // namespace vat::registry
