// local: the device itself — status, display/audio settings, clipboard,
// memory notes.  Targets: device://local/*, memory://* except what the
// memory package owns (context::owns_target).
#include "../common.h"
#include "../config.h"
#include "../context.h"
#include "../ops_local.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "本机资源规范：电量 device://local/battery，屏幕电源 device://local/display/power，音量 device://local/audio/volume，剪贴板 device://local/clipboard。最近任务为 memory://recent-tasks，记忆笔记为 memory://notes，用 write mode=append，content 只写要记住的事本身，去掉“记住”和句末标点，例如“记住明天检查服务器证书。”的 content 是 明天检查服务器证书。本机资源规范补充：查看本机状态用 read target=device://local/status；查看本机设置用 device://local/settings；设置屏幕亮度用 write target=device://local/display/brightness，content 为 8 到 255 的整数；设置自动息屏时间用 write target=device://local/display/timeout，content 为秒数。";

class LocalPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    bool reversible(const String&, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        return target.startsWith("device://local/display/") || target.startsWith("device://local/audio/") ||
               target == "device://local/clipboard" || target == "memory://notes";
    }

    const Manifest& manifest() const override {
        static const Manifest m{"local", "Cardputer 本机状态、设置、剪贴板与记忆",
                                "本机 设备 状态 电量 亮度 息屏 屏幕 音量 剪贴板 记忆 历史", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (tool == "read" || tool == "write") {
            return target.startsWith("device://local/") ||
                   (target.startsWith("memory://") && !context::owns_target(target));
        }
        return false;
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        if (tool == "read") return local::tool_read(arguments);
        return local::tool_write(arguments);
    }

    void triggers(std::vector<TriggerSpec>& out) const override {
        out.push_back({"device://boot", "每次开机就绪后"});
    }

    bool watch(const String& target, String& normalized, String& error) override {
        if (target != "device://boot") return false;
        normalized = target;  // emitted at every boot anyway
        return true;
    }

    void hints(std::vector<Hint>& out) override {
        const auto& cfg = config::get();
        out.push_back({"亮度", String(cfg.brightness), "把亮度调到 80"});
        out.push_back({"息屏", cfg.screen_timeout_seconds ? String(cfg.screen_timeout_seconds) + " 秒" : String("永不"),
                       "三分钟后息屏"});
        out.push_back({"音量", String(cfg.volume) + "%", "音量调到 30"});
        out.push_back({"记忆", "", "记住明天检查证书"});
        out.push_back({"剪贴板", "", "把部署完成复制到剪贴板"});
    }

    void resources(cJSON* list, int& remaining) override {
        cJSON* targets = cJSON_Parse(local::resources_json().c_str());
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, targets) {
            if (!add_resource(list, remaining, entry->valuestring, "local")) break;
        }
        cJSON_Delete(targets);
    }
};

}  // namespace

Package* make_local_package() { return new LocalPackage(); }

}  // namespace vat::registry
