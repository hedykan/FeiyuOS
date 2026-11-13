// lua: code mode.  write lua://run with a script as content runs it once,
// without saving it (programs::run_script): loops, waits and computing on
// what was read in one call instead of one model round per read.
#include "../common.h"
#include "../prog_runtime.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr char kUsage[] = "代码模式：要循环、等待或按读到的值计算时（读 10 次取平均、每隔几秒读一次、读完再按结果做下一步），write target=lua://run，content 写一段 Lua 一次跑完，不要一轮读一次，也不要存成程序。脚本里 vat.read、vat.write、vat.exec 和程序里一样，有数据时返回表（电量 .percent，加速度 .x .y .z，SSH 命令 .stdout），失败返回 nil 和原因；等待用 vat.sleep(毫秒)；print 的内容和 return 的值就是结果。要确认的调用会在设备上逐个问用户。几件互不相关的事直接并行调用工具，不写脚本。";

class LuaPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const Manifest& manifest() const override {
        static const Manifest m{"lua", "代码模式：写一段 Lua 一次跑完（循环、等待、按读到的值计算）", "lua 脚本 代码 循环 平均 多次 每隔", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "write" && json_str(arguments, "target") == "lua://run";
    }

    // Running a script is not itself a change: every vat call inside it goes
    // through Policy on its own, and one that needs confirmation asks the user.
    bool reversible(const String&, const cJSON*) const override { return true; }

    String execute(const String&, const cJSON* arguments, bool) override {
        return programs::run_script(json_str(arguments, "content"));
    }

};

}  // namespace

Package* make_lua_package() { return new LuaPackage(); }

}  // namespace vat::registry
