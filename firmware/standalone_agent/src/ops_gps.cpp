#include "ops_gps.h"

#include <TinyGPS++.h>
#include <cJSON.h>

#include "common.h"
#include "config.h"
#include "net.h"
#include "ops_lora.h"
#include "ops_ssh.h"

namespace vat::gps {
namespace {

constexpr uint32_t kBaud = 115200;
constexpr int kBytesPerLoop = 512;  // NMEA arrives at ~1 KB/s; never hold the UI loop long

HardwareSerial port(1);
TinyGPSPlus parser;
// Satellites in view, per constellation (GSV field 3); GGA only counts those in use.
TinyGPSCustom in_view_gp(parser, "GPGSV", 3);
TinyGPSCustom in_view_bd(parser, "BDGSV", 3);
TinyGPSCustom in_view_gb(parser, "GBGSV", 3);
TinyGPSCustom in_view_gl(parser, "GLGSV", 3);
TinyGPSCustom in_view_ga(parser, "GAGSV", 3);
bool running = false;
String off_reason = "未开启";

int in_view() {
    int total = 0;
    for (TinyGPSCustom* field : {&in_view_gp, &in_view_bd, &in_view_gb, &in_view_gl, &in_view_ga}) {
        if (field->isValid()) total += atoi(field->value());
    }
    return total;
}

String two(int value) { return value < 10 ? "0" + String(value) : String(value); }

}  // namespace

String begin() {
    off_reason = "";
    if (!lora::cap_present()) off_reason = "未检测到 Cap LoRa-1262";
    else if (!config::get().gps_enabled) off_reason = "未开启";
    if (!off_reason.isEmpty()) {
        if (running) port.end();
        running = false;
        return off_reason;
    }
    if (!running) {
        // ~2 s of NMEA: the UI loop sometimes draws for a while; with the
        // default 256 bytes sentences were dropped (36 failed checksums in 48 KB).
        port.setRxBufferSize(2048);
        port.begin(kBaud, SERIAL_8N1, kRx, kTx);
    }
    running = true;
    return "";
}

void loop() {
    if (!running) return;
    for (int n = 0; n < kBytesPerLoop && port.available() > 0; ++n) parser.encode(port.read());
}

bool location(double& lat, double& lon) {
    if (!running || !parser.location.isValid() || parser.location.age() >= 10000) return false;
    lat = parser.location.lat();
    lon = parser.location.lng();
    return true;
}

String status_text() {
    if (!running) return "GPS：关闭（" + off_reason + "）";
    if (parser.charsProcessed() == 0) {
        return "GPS：开启，但还没收到模块的任何输出（检查 Cap 是否插好；开机后约 1 秒开始输出）";
    }
    String text;
    const bool fixed = parser.location.isValid() && parser.location.age() < 5000;
    if (fixed) {
        text = "GPS：已定位\n位置 " + String(parser.location.lat(), 6) + ", " + String(parser.location.lng(), 6);
        if (parser.hdop.isValid()) text += "（HDOP " + String(parser.hdop.hdop(), 1) + "）";
        if (parser.altitude.isValid()) text += "，海拔 " + String(parser.altitude.meters(), 1) + " m";
        if (parser.speed.isValid() && parser.speed.kmph() > 1) text += "，速度 " + String(parser.speed.kmph(), 1) + " km/h";
    } else {
        text = "GPS：未定位（室内通常无法定位；室外空旷处冷启动约 30–60 秒）";
    }
    text += "\n卫星：在用 " + String(parser.satellites.isValid() ? parser.satellites.value() : 0) + " 颗，可见 " +
            String(in_view()) + " 颗";
    if (parser.date.isValid() && parser.time.isValid() && parser.date.year() > 2000) {
        text += "\n卫星时间 " + String(parser.date.year()) + "-" + two(parser.date.month()) + "-" + two(parser.date.day()) +
                " " + two(parser.time.hour()) + ":" + two(parser.time.minute()) + ":" + two(parser.time.second()) + " UTC";
    }
    text += "\n已收 NMEA " + String(parser.charsProcessed()) + " 字节，校验失败 " + String(parser.failedChecksum());
    return text;
}

String address_text() {
    if (!running) return status_text();
    if (!(parser.location.isValid() && parser.location.age() < 5000)) {
        return "还没有定位，查不了地址。\n" + status_text();
    }
    static String cached;
    static String cached_key;
    static unsigned long cached_at = 0;
    const String lat = String(parser.location.lat(), 5);
    const String lon = String(parser.location.lng(), 5);
    const String key = lat + "," + lon;
    if (key == cached_key && millis() - cached_at < 60000) return cached;
    ssh::release();  // TLS and an SSH session never coexist
    const auto response = net::get("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=18"
                                   "&accept-language=zh-CN&lat=" + lat + "&lon=" + lon,
                                   "", 12000);
    if (!response.ok()) return "地址查询失败（HTTP " + String(response.status) + "）";
    cJSON* body = cJSON_Parse(response.body.c_str());
    if (!body) return "地址查询结果无法解析";
    const cJSON* address = json_obj(body, "address");
    String parts;
    for (const char* field : {"road", "neighbourhood", "suburb", "city_district", "district", "city", "state"}) {
        const String value = json_str(address, field);
        if (!value.isEmpty() && parts.indexOf(value) < 0) parts += (parts.isEmpty() ? "" : "，") + value;
    }
    const String place = json_str(body, "name");
    cJSON_Delete(body);
    if (parts.isEmpty()) return "这里没有可用的地址信息";
    cached = "当前位置：" + parts + (place.isEmpty() || parts.indexOf(place) >= 0 ? String() : "（附近：" + place + "）") +
             "\n（地址来自 OpenStreetMap，适合用来搜索附近的地方）";
    cached_key = key;
    cached_at = millis();
    return cached;
}

String enable(bool on) {
    config::get().gps_enabled = on;
    if (!config::save_value("gps", "enabled", cJSON_CreateBool(on))) return "保存失败";
    const String problem = begin();
    if (!on) return "GPS 已关闭（停止读取；模块本身仍由 Cap 供电）";
    return problem.isEmpty() ? "GPS 已开启。\n" + status_text() : "GPS 未能开启：" + problem;
}

}  // namespace vat::gps
