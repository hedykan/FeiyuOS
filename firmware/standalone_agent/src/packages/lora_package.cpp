// lora: the Cap LoRa-1262 radio.  Targets:
//   read  lora://status                 state, settings, last packet
//   read  lora://messages               received and sent messages (numbered)
//   write lora://send                   send content (confirmation; needs frequency + confirmed antenna)
//   write lora://settings/<key>         enabled|frequency|power|sf|bandwidth|cr|antenna (confirmation)
//   session connect|attach lora://      turn receiving on
//   session disconnect lora://          off: the SX1262 is held in reset, off the shared SPI bus
#include "../common.h"
#include "../ops_lora.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// lora://… as is; device://lora/… and device://local/lora/… (what models guess
// when they skip discover) mapped onto it.  "" = not a LoRa target.
String canonical(const String& target, const String& content) {
    String path = target_path(target);
    if (path.startsWith("lora://")) return path;
    String rest;
    if (path.startsWith("device://lora")) rest = path.substring(13);
    else if (path.startsWith("device://local/lora")) rest = path.substring(19);
    else return "";
    if (rest.startsWith("/")) rest = rest.substring(1);
    if (rest.isEmpty() || rest == "status") return "lora://status";
    if (rest == "messages" || rest == "inbox") return "lora://messages";
    if (rest == "send" || rest == "transmit" || rest == "message" || rest == "tx") return "lora://send";
    // "power" is on/off when the content says so, dBm when it is a number.
    if (rest == "power" || rest == "state" || rest == "enabled" || rest == "receive") {
        const bool number = content.length() && (isdigit(static_cast<unsigned char>(content[0])) || content[0] == '-');
        return number && rest == "power" ? "lora://settings/power" : "lora://settings/enabled";
    }
    if (rest == "freq") rest = "frequency";
    if (rest == "spreading_factor") rest = "sf";
    return "lora://settings/" + rest;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "LoRa 用 lora:// 而不是 device://：开关 write lora://settings/enabled（content on 或 off），频点、功率写 lora://settings/frequency、lora://settings/power，发消息 write lora://send，状态 read lora://status，收到的消息 read lora://messages。";

class LoraPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"lora", "Cap LoRa-1262：LoRa 收发、频点与天线设置",
                                "lora LoRa 无线 电台 节点 离线 消息 发消息 收消息 频点 频率 天线 cap", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        if (canonical(json_str(arguments, "target"), json_str(arguments, "content")).isEmpty()) return false;
        return tool == "read" || tool == "write" || tool == "session";
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String path = canonical(json_str(arguments, "target"), json_str(arguments, "content"));
        if (tool == "session") {
            const String action = json_str(arguments, "action");
            if (action == "connect" || action == "attach" || action == "create") return lora::enable(true);
            if (action == "disconnect") return lora::enable(false);
            return lora::status_text();
        }
        if (tool == "write") {
            const String content = json_str(arguments, "content");
            if (path == "lora://send") return lora::send(content);
            if (path.startsWith("lora://settings/")) return lora::set(path.substring(16), content);
            return "LoRa 只能写 lora://send 或 lora://settings/<项>";
        }
        if (path == "lora://messages") return lora::messages_text();
        return lora::status_text();
    }

    void hints(std::vector<Hint>& out) override {
        if (!lora::cap_present()) return;
        const String status = lora::status_text();
        out.push_back({"LoRa", status.substring(0, status.indexOf('\n')), "打开 LoRa 接收"});
        out.push_back({"LoRa 消息", "", "看看 LoRa 收到了什么"});
    }

    void resources(cJSON* list, int& remaining) override {
        if (!lora::cap_present()) return;
        for (const char* target : {"lora://status", "lora://messages", "lora://settings/enabled", "lora://send",
                                   "lora://settings/frequency"}) {
            add_resource(list, remaining, target, "lora");
        }
    }
};

}  // namespace

Package* make_lora_package() { return new LoraPackage(); }

}  // namespace vat::registry
