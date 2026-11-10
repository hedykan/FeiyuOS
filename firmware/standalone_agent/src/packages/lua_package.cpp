// lua: prototype runner (branch feat/lua-prog).  write lua://run with a script
// as content runs it and returns its output with what it cost.  Not in the
// system prompt yet: tried through the web API while the bindings settle.
#include <esp_heap_caps.h>

#include "../common.h"
#include "../lua_vm.h"
#include "../registry.h"

namespace vat::registry {
namespace {

class LuaPackage : public Package {
public:
    const Manifest& manifest() const override {
        static const Manifest m{"lua", "Lua 脚本（验证中）", "lua 脚本", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "write" && json_str(arguments, "target") == "lua://run";
    }

    // Running a script is not itself a change: every vat call inside it goes
    // through Policy on its own, and one that would ask is refused.
    bool reversible(const String&, const cJSON*) const override { return true; }

    String execute(const String&, const cJSON* arguments, bool) override {
        const std::size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        String output, result, error;
        luavm::Stats stats;
        luavm::Options none;
        const bool ok = luavm::run(json_str(arguments, "content"), "script", none, output, result, error, stats);
        if (!result.isEmpty()) output += "return: " + result + "\n";
        const std::size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const String cost = "lua: peak=" + String(static_cast<unsigned>(stats.peak)) + " arena=" + String(static_cast<unsigned>(stats.arena)) + " instructions=" +
                            String(stats.instructions) + " ms=" + String(stats.ms) + " heap_before=" +
                            String(static_cast<unsigned>(before)) + " heap_after=" + String(static_cast<unsigned>(after));
        runtime_event("lua_run", cost + (ok ? "" : " error"));
        if (!ok) return "ERROR: " + error + "\n" + output + cost;
        return output + cost;
    }
};

}  // namespace

Package* make_lua_package() { return new LuaPackage(); }

}  // namespace vat::registry
