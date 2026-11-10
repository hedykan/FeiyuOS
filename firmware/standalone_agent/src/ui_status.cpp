#include "ui_status.h"

#include <M5Cardputer.h>
#include <time.h>

#include <algorithm>

#include "config.h"
#include "net.h"
#include "ops_ble.h"

namespace vat::ui {

void wifi_bars(Gfx& g, int x, int y) {
    const bool online = net::state() == net::WifiState::Connected;
    const int rssi = net::rssi();
    const int level = !online ? 0 : rssi > -60 ? 3 : rssi > -75 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        const int h = 3 + i * 2;
        const uint16_t color = !online ? kDanger : i < level ? kAccent : kMuted;
        g.fillRect(x + i * 4, y + 9 - h, 3, h, color);
    }
}

namespace status {
namespace {

constexpr int kSignalWidth = 12;
constexpr int kGap = 6;
String drawn;  // signature of what draw() last showed
uint32_t checked_ms = 0;

String time_text() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "--:--";
    char text[8];
    strftime(text, sizeof text, "%H:%M", &t);
    return text;
}

int battery() { return M5Cardputer.Power.getBatteryLevel(); }

int signal_level() {
    if (net::state() != net::WifiState::Connected) return 0;
    const int rssi = net::rssi();
    return rssi > -60 ? 3 : rssi > -75 ? 2 : 1;
}

// The text an item shows ("" = nothing now; the signal is an icon).
String text_of(const String& id) {
    if (id == "time") return time_text();
    if (id == "battery") return String(battery()) + "%";
    if (id == "ssid") return net::ssid();
    if (id == "heap") return String(ESP.getFreeHeap() / 1024) + "K";
    if (id == "ble") return ble::running() ? "BT" : "";
    return "";
}

// What would be drawn, coarse enough not to redraw for noise (battery in
// 5 % steps, free memory in 4 KB steps).
String signature() {
    String s;
    for (const auto& id : chosen()) {
        if (id == "signal") s += String(signal_level());
        else if (id == "battery") s += String(battery() / 5);
        else if (id == "heap") s += String(ESP.getFreeHeap() / 4096);
        else s += text_of(id);
        s += '|';
    }
    return s;
}

}  // namespace

const std::vector<Item>& all() {
    static const std::vector<Item> items = {{"time", "时间"},    {"battery", "电量"}, {"signal", "Wi-Fi 信号"},
                                            {"ssid", "Wi-Fi 名称"}, {"heap", "剩余内存"}, {"ble", "蓝牙"}};
    return items;
}

std::vector<String> chosen() {
    std::vector<String> ids;
    const String& setting = config::get().status_items;
    int from = 0;
    while (from <= static_cast<int>(setting.length())) {
        int comma = setting.indexOf(',', from);
        if (comma < 0) comma = setting.length();
        String id = setting.substring(from, comma);
        id.trim();
        for (const auto& item : all()) {
            if (id == item.id && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        from = comma + 1;
    }
    return ids;
}

bool save(const std::vector<String>& ids) {
    String setting;
    for (const auto& id : ids) setting += (setting.isEmpty() ? "" : ",") + id;
    config::get().status_items = setting;
    drawn = "";  // redraw the bar
    return config::save_value("ui", "status_items", cJSON_CreateString(setting.c_str()));
}

String summary() {
    String out;
    for (const auto& id : chosen()) {
        for (const auto& item : all()) {
            if (id == item.id) out += String(out.isEmpty() ? "" : "·") + (id == "signal" ? "信号" : id == "ssid" ? "WiFi名" : item.label);
        }
    }
    return out.isEmpty() ? String("不显示") : out;
}

void draw(Gfx& g, int top, int right, int left, uint16_t bg) {
    drawn = signature();
    for (const auto& id : chosen()) {
        if (id == "signal") {
            if (right - kSignalWidth < left) break;
            wifi_bars(g, right - kSignalWidth, top + 3);
            right -= kSignalWidth + kGap;
            continue;
        }
        String text = text_of(id);
        if (text.isEmpty()) continue;
        if (id == "ssid") text = fit(g, text, 64);
        const int w = g.textWidth(text);
        if (right - w < left) break;
        uint16_t color = kMuted;
        if (id == "time") color = kText;
        if (id == "battery" && battery() < 20) color = kWarn;
        if (id == "ble") color = kAccent;
        g.setTextColor(color, bg);
        g.drawString(text, right - w, top + 2);
        right -= w + kGap;
    }
}

bool changed() {
    if (millis() - checked_ms < 1000) return false;
    checked_ms = millis();
    return signature() != drawn;
}

}  // namespace status
}  // namespace vat::ui
