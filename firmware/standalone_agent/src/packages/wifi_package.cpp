// wifi: nearby networks and connection management.
//   read  wifi://scan            nearby networks (ssid, rssi, secured, channel)
//   read  wifi://status          current network
//   write wifi://SSID            save + switch (Policy: ask); the password is
//                                typed on the device, never taken from the model
//   session connect wifi://NAME  switch to a saved network
// A failed switch always returns to the previous network.
#include <WiFi.h>

#include <algorithm>

#include "../common.h"
#include "../config.h"
#include "../net.h"
#include "../ops_wifi.h"
#include "../registry.h"
#include "../storage.h"

namespace vat::registry {
namespace {

String status() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", net::ssid().c_str());
    cJSON_AddStringToObject(root, "ip", net::ip().c_str());
    cJSON_AddNumberToObject(root, "rssi", net::rssi());
    cJSON* saved = cJSON_AddArrayToObject(root, "saved");
    for (const auto& network : config::get().wifi_networks) {
        cJSON_AddItemToArray(saved, cJSON_CreateString(network.name.c_str()));
    }
    const String json = json_print(root);
    cJSON_Delete(root);
    return json;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "WiFi 规范：搜索、扫描附近 WiFi 用 read target=wifi://scan；查看当前网络用 read target=wifi://status；连接 WiFi（无论是否保存过）用 session(action=connect,target=wifi://网络名)，网络名逐字保留，包括用户说出的 WiFi 后缀，例如连接家里 WiFi 是 target=wifi://家里 WiFi。";

class WifiPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"wifi", "WiFi 扫描、状态与切换（含语音设置新网络）",
                                "wifi 无线 网络 热点 信号 扫描 搜索 附近 连接 密码", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (!target.startsWith("wifi://")) return false;
        if (tool == "read" || tool == "write") return true;
        const String action = json_str(arguments, "action");
        return tool == "session" && (action == "connect" || action == "disconnect");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String name = target.substring(7);
        if (tool == "read") {
            if (name == "scan" || name.isEmpty()) return wifi::scan_text();
            if (name == "status" || name == "current") return status();
            return "未知 WiFi 资源：" + target + "（可用 wifi://scan、wifi://status）";
        }
        if (json_str(arguments, "action") == "disconnect") return "不支持断开 WiFi（设备需要网络）";
        // Any password in the model's arguments is ignored on purpose: secrets
        // only come from the device's masked input (execute_secret).
        if (wifi::saved(name)) return net::ssid() == name ? "已连接 WiFi " + name : wifi::join_saved(name);
        bool ok = false;
        if (!wifi::secured(name)) return wifi::join(name, "", ok);
        return "连接 WiFi " + name + " 需要在设备上输入密码";
    }

    String needs_secret(const String& tool, const cJSON* arguments) override {
        const String target = json_str(arguments, "target");
        const bool connecting = tool == "write" || (tool == "session" && json_str(arguments, "action") == "connect");
        if (!connecting) return {};
        const String name = target.substring(7);
        if (name.isEmpty() || wifi::saved(name) || !wifi::secured(name)) return {};
        return "WiFi「" + name + "」的密码";
    }

    String execute_secret(const String& tool, const cJSON* arguments, bool, const String& secret) override {
        bool ok = false;
        return wifi::join(json_str(arguments, "target").substring(7), secret, ok);
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"WiFi", net::ssid(), "搜索附近的 WiFi"});
        out.push_back({"切换网络", "", "连接 WiFi 网络名（密码在设备上输入）"});
    }

    void resources(cJSON* list, int& remaining) override {
        for (const char* target : {"wifi://scan", "wifi://status"}) {
            add_resource(list, remaining, target, "wifi");
        }
    }
};

}  // namespace

Package* make_wifi_package() { return new WifiPackage(); }

}  // namespace vat::registry
