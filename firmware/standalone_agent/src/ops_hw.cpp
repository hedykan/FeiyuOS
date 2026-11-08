#include "ops_hw.h"

#include <M5Cardputer.h>
#include <driver/usb_serial_jtag.h>

#include <cmath>

#include "config.h"
#include "ui_state.h"

namespace vat::hw {
namespace {

constexpr uint32_t kSampleMs = 30000;
constexpr int kSamples = 10;  // 5 minutes of history
int samples[kSamples] = {0};
int sample_count = 0;
int sample_next = 0;
uint32_t last_sample = 0;
String led_color = "off";

int trend_mv() {  // newest minus oldest sample
    if (sample_count < 4) return 0;
    const int newest = samples[(sample_next + kSamples - 1) % kSamples];
    const int oldest = samples[sample_count < kSamples ? 0 : sample_next];
    return newest - oldest;
}

bool parse_color(String text, uint8_t& r, uint8_t& g, uint8_t& b) {
    text.trim();
    text.toLowerCase();
    struct Named { const char* name; uint32_t rgb; };
    static const Named named[] = {
        {"red", 0xFF0000}, {"红", 0xFF0000}, {"green", 0x00FF00}, {"绿", 0x00FF00}, {"blue", 0x0000FF},
        {"蓝", 0x0000FF}, {"yellow", 0xFFC000}, {"黄", 0xFFC000}, {"orange", 0xFF6000}, {"橙", 0xFF6000},
        {"purple", 0xA000FF}, {"紫", 0xA000FF}, {"cyan", 0x00FFFF}, {"青", 0x00FFFF}, {"white", 0xFFFFFF},
        {"白", 0xFFFFFF}, {"pink", 0xFF4080}, {"粉", 0xFF4080}, {"off", 0}, {"关", 0}, {"black", 0}, {"on", 0xFFFFFF}, {"开", 0xFFFFFF},
    };
    uint32_t rgb = 0;
    bool found = false;
    if (text.startsWith("#") && text.length() == 7) {
        char* end = nullptr;
        rgb = strtoul(text.c_str() + 1, &end, 16);
        found = end && *end == 0;
    }
    for (const auto& n : named) {
        if (!found && text.indexOf(n.name) >= 0) {
            rgb = n.rgb;
            found = true;
        }
    }
    r = rgb >> 16;
    g = rgb >> 8;
    b = rgb;
    return found;
}

}  // namespace

void loop() {
    if (last_sample && millis() - last_sample < kSampleMs) return;
    last_sample = millis();
    samples[sample_next] = battery_mv();
    sample_next = (sample_next + 1) % kSamples;
    if (sample_count < kSamples) ++sample_count;
}

int battery_mv() { return M5Cardputer.Power.getBatteryVoltage(); }

bool usb_connected() { return usb_serial_jtag_is_connected(); }

String battery_text() {
    const int mv = battery_mv();
    const int trend = trend_mv();
    String charging;
    if (usb_connected()) {
        charging = mv >= 4150 ? "USB 已连接，接近充满（推断）" : "USB 已连接，充电中（推断）";
    } else if (trend >= 30) {
        charging = "电压在上升，可能在充电（推断）";
    } else {
        charging = "未连接 USB 电脑；用充电头充电时本机无法直接判断";
    }
    String text = "电量 " + String(M5Cardputer.Power.getBatteryLevel()) + "%，电压 " + String(mv / 1000.0, 2) + " V；" + charging;
    if (sample_count >= 4) text += "；近 " + String(sample_count * kSampleMs / 60000) + " 分钟电压变化 " + String(trend) + " mV";
    return text;
}

String set_led(const String& color) {
    uint8_t r, g, b;
    if (!parse_color(color, r, g, b)) return "ERROR: 不认识的颜色，可用：红/绿/蓝/黄/橙/紫/青/白/粉、#RRGGBB 或 off";
    M5.Led.setBrightness(64);  // full brightness is glaring and draws ~40 mA
    M5.Led.setAllColor(m5gfx::rgb888_t(r, g, b));
    M5.Led.display();
    led_color = (r | g | b) ? color : String("off");
    return (r | g | b) ? "RGB 灯已设为 " + color : String("RGB 灯已关闭");
}

String led_text() { return "RGB 灯：" + led_color; }

String imu_text() {
    if (!M5.Imu.isEnabled()) return "ERROR: IMU（BMI270）未初始化";
    float ax, ay, az, gx, gy, gz;
    M5.Imu.update();
    if (!M5.Imu.getAccel(&ax, &ay, &az) || !M5.Imu.getGyro(&gx, &gy, &gz)) return "ERROR: IMU 读取失败";
    const float g = std::sqrt(ax * ax + ay * ay + az * az);
    const float spin = std::sqrt(gx * gx + gy * gy + gz * gz);
    const float pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az)) * 180 / PI;
    const float roll = std::atan2(ay, az) * 180 / PI;
    const char* face = az > 0.8f ? "屏幕朝上平放" : az < -0.8f ? "屏幕朝下" : std::fabs(ax) > std::fabs(ay) ? "竖立（侧边朝下）" : "竖立（上/下边朝下）";
    const bool moving = std::fabs(g - 1.0f) > 0.08f || spin > 8.0f;
    char text[220];
    snprintf(text, sizeof(text), "%s，%s；俯仰 %.0f°，横滚 %.0f°；加速度 %.2f/%.2f/%.2f g，角速度 %.0f°/s", face,
             moving ? "正在移动或晃动" : "静止", pitch, roll, ax, ay, az, spin);
    return text;
}

bool imu_latest(float accel[3], float gyro[3]) {
    static float a[3], g[3];
    static unsigned long read_at = 0;
    static bool ok = false;
    if (!M5.Imu.isEnabled()) return false;
    if (!ok || millis() - read_at >= 5) {
        M5.Imu.update();
        ok = M5.Imu.getAccel(&a[0], &a[1], &a[2]) && M5.Imu.getGyro(&g[0], &g[1], &g[2]);
        read_at = millis();
    }
    for (int i = 0; i < 3; ++i) {
        accel[i] = a[i];
        gyro[i] = g[i];
    }
    return ok;
}

String request_beep(int count) {
    count = std::max(1, std::min(count, 5));
    ui::post_beep(count);
    return "已响 " + String(count) + " 声提示音";
}

void play_pending_beeps() {
    int count = 0;
    if (!ui::take_beep(count) || count <= 0) return;
    if (M5Cardputer.Mic.isRunning()) return;  // recording owns I2S; a beep now would cut it
    auto& spk = M5Cardputer.Speaker;
    if (!spk.begin()) return;
    spk.setVolume(static_cast<uint8_t>(std::min(255, config::get().volume * 255 / 100)));
    for (int i = 0; i < count; ++i) {
        spk.tone(2200, 90);
        while (spk.isPlaying()) delay(5);
        delay(80);
    }
    spk.end();
}

// The model often guesses light/rgb instead of discovering led.
bool is_led_target(String target) {
    target.toLowerCase();
    for (const char* name : {"device://local/led", "device://local/light", "device://local/rgb"}) {
        if (target.startsWith(name)) return true;
    }
    return false;
}
bool is_beep_target(const String& target) {
    return target == "device://local/audio/beep" || target == "device://local/beep" || target == "device://local/audio/tone";
}
}  // namespace vat::hw
