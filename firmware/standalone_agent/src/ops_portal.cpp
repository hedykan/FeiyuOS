#include "ops_portal.h"

#include <DNSServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include "net.h"

namespace vat::portal {
namespace {

constexpr uint32_t kIdleMs = 10UL * 60 * 1000;
DNSServer dns;
bool running = false;
bool station_was_up = false;
String hotspot;
String secret;
uint32_t last_request = 0;

String escape_qr(const String& text) {  // WIFI: QR syntax escapes \ ; , : "
    String out;
    for (const char c : text) {
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

bool start() {
    if (running) {
        touch();
        return true;
    }
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[16];
    snprintf(name, sizeof(name), "VAT-%02X%02X", mac[4], mac[5]);
    hotspot = name;
    char digits[9];
    snprintf(digits, sizeof(digits), "%08lu", static_cast<unsigned long>(esp_random() % 100000000UL));
    secret = digits;
    station_was_up = WiFi.status() == WL_CONNECTED;
    WiFi.mode(station_was_up ? WIFI_AP_STA : WIFI_AP);
    // One radio: the hotspot has to sit on the station's channel.
    const int channel = station_was_up ? WiFi.channel() : 6;
    if (!WiFi.softAP(hotspot.c_str(), secret.c_str(), channel, 0, 2)) {
        secret = String();
        WiFi.mode(station_was_up ? WIFI_STA : WIFI_OFF);
        return false;
    }
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    running = true;
    touch();
    return true;
}

void stop() {
    if (!running) return;
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(station_was_up ? WIFI_STA : WIFI_OFF);
    running = false;
    secret = String();
}

bool active() { return running; }

void loop() {
    if (!running) return;
    dns.processNextRequest();
    if (millis() - last_request > kIdleMs) stop();
}

void touch() { last_request = millis(); }

String ssid() { return hotspot; }
String password() { return secret; }
int clients() { return running ? WiFi.softAPgetStationNum() : 0; }
String wifi_qr() { return "WIFI:T:WPA;S:" + escape_qr(hotspot) + ";P:" + escape_qr(secret) + ";;"; }
String url() { return "http://" + WiFi.softAPIP().toString() + "/m"; }

bool from_portal(const IPAddress& local) { return running && local == WiFi.softAPIP(); }

}  // namespace vat::portal
