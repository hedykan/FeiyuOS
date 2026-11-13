// lora: the Cap LoRa-1262 radio.  Addresses (docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §5):
//   read  device://local/lora/status        state, settings, last packet
//   read  device://local/lora/messages      received and sent messages (numbered)
//   write device://local/lora/send          send content (confirmation; needs frequency + confirmed antenna)
//   write settings://lora/<key>             enabled|frequency|power|sf|bandwidth|cr|antenna (confirmation)
//   session connect|attach device://local/lora   turn receiving on
//   session disconnect device://local/lora       off: the SX1262 is held in reset, off the shared SPI bus
#include "../common.h"
#include "../ops_lora.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr char kRadio[] = "device://local/lora";
constexpr char kSettings[] = "settings://lora/";

// What the target names: "status", "messages", "send", "settings/<key>", or "" when not LoRa.
String what(const String& target) {
    const String path = target_path(target);
    if (path.startsWith(kSettings)) return "settings/" + path.substring(strlen(kSettings));
    if (path == kRadio || path == String(kRadio) + "/") return "status";
    if (path.startsWith(String(kRadio) + "/")) return path.substring(strlen(kRadio) + 1);
    return "";
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "LoRa：开关接收 write settings://lora/enabled（content on 或 off），频点、功率 write settings://lora/frequency、settings://lora/power，发消息 write device://local/lora/send，状态 read device://local/lora/status，收到的消息 read device://local/lora/messages。";

class LoraPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override {
        return "read device://local/lora/status、device://local/lora/messages，write device://local/lora/send；开关、频点、功率 write settings://lora/enabled|frequency|power";
    }
    bool present() const override { return lora::cap_present(); }

    const Manifest& manifest() const override {
        static const Manifest m{"lora", "Cap LoRa-1262：LoRa 收发、频点与天线设置",
                                "lora LoRa 无线 电台 节点 离线 消息 发消息 收消息 频点 频率 天线 cap", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        if (what(json_str(arguments, "target")).isEmpty()) return false;
        return tool == "read" || tool == "write" || tool == "session";
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        if (!present()) return "ERROR: 未接上 Cap LoRa-1262";
        const String path = what(json_str(arguments, "target"));
        if (tool == "session") {
            const String action = json_str(arguments, "action");
            if (action == "connect" || action == "attach" || action == "create") return lora::enable(true);
            if (action == "disconnect") return lora::enable(false);
            return lora::status_text();
        }
        if (tool == "write") {
            const String content = json_str(arguments, "content");
            if (path == "send") return lora::send(content);
            if (path.startsWith("settings/")) return lora::set(path.substring(9), content);
            return "ERROR: LoRa 只能写 device://local/lora/send 或 settings://lora/<项>";
        }
        if (path == "messages") return lora::messages_text();
        if (path == "status") return lora::status_text();
        return "ERROR: LoRa 能读的是 device://local/lora/status 和 device://local/lora/messages";
    }

    void hints(std::vector<Hint>& out) override {
        if (!lora::cap_present()) return;
        const String status = lora::status_text();
        out.push_back({"LoRa", status.substring(0, status.indexOf('\n')), "打开 LoRa 接收"});
        out.push_back({"LoRa 消息", "", "看看 LoRa 收到了什么"});
    }

    void resources(cJSON* list, int& remaining) override {
        if (!lora::cap_present()) return;
        for (const char* target : {"device://local/lora/status", "device://local/lora/messages", "settings://lora/enabled",
                                   "device://local/lora/send", "settings://lora/frequency"}) {
            add_resource(list, remaining, target, "lora");
        }
    }
};

}  // namespace

Package* make_lora_package() { return new LoraPackage(); }

}  // namespace vat::registry
