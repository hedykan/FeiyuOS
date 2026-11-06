#include "ops_wifi.h"

#include <WiFi.h>
#include <cJSON.h>

#include <algorithm>

#include "common.h"
#include "config.h"
#include "net.h"
#include "storage.h"
#include "wifi_saved.h"

namespace vat::wifi {
namespace {

std::vector<Network> last_scan;

// `ssid` becomes the primary network and stays in the saved list; the primary
// it replaces is kept too (see wifi_saved.h).
bool remember(const String& ssid, const String& password) {
    cJSON* secrets = cJSON_Parse(storage::read_text("/vat/secrets.json", 32768).c_str());
    if (!secrets) secrets = cJSON_CreateObject();
    cJSON* wifi = json_obj(secrets, "wifi");
    if (!wifi) wifi = cJSON_AddObjectToObject(secrets, "wifi");
    wifi_saved::remember(wifi, ssid, password);
    char* printed = cJSON_Print(secrets);
    cJSON_Delete(secrets);
    const bool ok = printed && storage::write_text("/vat/secrets.json", String(printed));
    if (printed) cJSON_free(printed);
    if (ok) config::load();
    return ok;
}

}  // namespace

String signal_word(int rssi) { return rssi > -60 ? "强" : rssi > -75 ? "中" : "弱"; }

std::vector<Network> scan(std::size_t max) {
    const int count = WiFi.scanNetworks(false, false);
    std::vector<Network> found;
    if (count <= 0) {
        WiFi.scanDelete();
        last_scan = found;
        return found;
    }
    std::vector<int> order;
    for (int i = 0; i < count; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
    for (const int i : order) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        if (std::any_of(found.begin(), found.end(), [&](const Network& n) { return n.ssid == ssid; })) continue;
        if (found.size() >= max) break;
        found.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
    }
    WiFi.scanDelete();
    last_scan = found;
    return found;
}

String scan_text() {
    const auto networks = scan();
    if (networks.empty()) return "附近没有可见的 WiFi";
    String text = "附近 " + String(networks.size()) + " 个 WiFi：\n";
    int n = 0;
    for (const auto& network : networks) {
        text += String(++n) + ". " + network.ssid + "  信号" + signal_word(network.rssi) +
                (network.secured ? "  加密" : "  开放") + (network.ssid == net::ssid() ? "  当前连接" : "") + "\n";
    }
    return text;
}

bool secured(const String& ssid) {
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& network : last_scan) {
            if (network.ssid == ssid) return network.secured;
        }
        if (pass == 0) scan();
    }
    return true;  // not visible: assume a password is needed
}

bool saved(const String& name) {
    for (const auto& network : config::get().wifi_networks) {
        if (network.name == name || network.ssid == name) return true;
    }
    return name == config::get().wifi_ssid;
}

String join(const String& ssid, const String& password, bool& ok) {
    ok = false;
    if (ssid.isEmpty()) return "缺少 WiFi 名称";
    const String previous_ssid = config::get().wifi_ssid;
    const String previous_password = config::get().wifi_password;
    if (!net::connect_station(ssid, password, 15000)) {
        if (!previous_ssid.isEmpty() && previous_ssid != ssid) net::connect_station(previous_ssid, previous_password, 15000);
        return "连接「" + ssid + "」失败：密码不对或信号太弱。没有保存，" +
               (net::ssid().isEmpty() ? String("当前未联网") : "已回到「" + net::ssid() + "」");
    }
    ok = true;
    if (!remember(ssid, password)) return "已连接「" + ssid + "」，但保存失败（下次开机不会自动连接）";
    return "已连接「" + ssid + "」，IP " + net::ip() + "，已设为开机默认网络";
}

String join_saved(const String& name) {
    for (const auto& network : config::get().wifi_networks) {
        if (network.name == name || network.ssid == name) {
            bool ok = false;
            return join(network.ssid, network.password, ok);
        }
    }
    if (name == config::get().wifi_ssid) {
        bool ok = false;
        return join(name, config::get().wifi_password, ok);
    }
    return "没有保存的 WiFi 配置: " + name;
}

bool connect_at_boot(uint32_t timeout_ms) {
    const auto& cfg = config::get();
    if (!cfg.wifi_ssid.isEmpty() && net::connect_station(cfg.wifi_ssid, cfg.wifi_password, timeout_ms)) return true;
    for (const auto& network : cfg.wifi_networks) {
        if (network.ssid == cfg.wifi_ssid) continue;
        runtime_event("wifi", "primary failed, trying saved " + network.ssid);
        if (net::connect_station(network.ssid, network.password, 10000)) return true;
    }
    return false;
}

}  // namespace vat::wifi
