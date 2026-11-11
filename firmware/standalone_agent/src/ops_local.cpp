#include "ops_local.h"

#include <M5Cardputer.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <fcntl.h>
#include <lwip/sockets.h>

#include "common.h"
#include "config.h"
#include "context.h"
#include "net.h"
#include "ops_hw.h"
#include "registry.h"
#include "storage.h"
#include "ui_state.h"
#include "worker.h"

namespace vat::local {

bool owns(const String& target) {
    return target.startsWith("device://local/") ||
           (target.startsWith("memory://") && !context::owns_target(target));
}

int open_sockets() {
    int count = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
        if (fcntl(fd, F_GETFL) != -1) ++count;
    }
    return count;
}

namespace {

// CPU load from the idle tasks' run time (esp_timer microseconds, a 32-bit
// counter that wraps every ~71 min; unsigned differences over 10 s are fine).
constexpr unsigned long kCpuWindowMs = 10000;
unsigned long cpu_window_start = 0;
uint32_t idle_before[2] = {0, 0};
uint32_t wall_before = 0;
int idle_percent[2] = {-1, -1};      // last window
int idle_percent_min[2] = {101, 101};  // busiest window since boot

// Storage growth since the first status request (FAT32 caches the free count,
// so usedBytes() is cheap after its first call).
uint64_t used_baseline = 0;
unsigned long used_baseline_ms = 0;

String cpu_text(const int (&percent)[2]) {
    if (percent[0] < 0 || percent[0] > 100) return "";
    return "c0 " + String(percent[0]) + "% c1 " + String(percent[1]) + "%";
}
String cap_bus = "unprobed";

String board_name() {
    switch (M5.getBoard()) {
        case m5::board_t::board_M5Cardputer: return "cardputer";
        case m5::board_t::board_M5CardputerADV: return "cardputer_adv";
        default: return "other:" + String(static_cast<int>(M5.getBoard()));
    }
}

// scanID checks the ACK; start() only reports that a transaction could
// begin, and every address "answered" (the cap looked present when removed).
bool answers(m5::I2C_Class& bus, uint8_t address) { return bus.isEnabled() && bus.scanID(address, 100000); }
}  // namespace

String i2c_internal_devices;  // DIAG: every address answering on G8/G9 at boot

void probe_hardware() {
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (answers(M5.In_I2C, address)) {
            char hex[6];
            snprintf(hex, sizeof(hex), "0x%02x ", address);
            i2c_internal_devices += hex;
        }
    }
    // Cap LoRa-1262 carries a PI4IOE5V6408 I/O expander at 0x43; which bus
    // it answers on is not settled across units, so probe both once at boot.
    constexpr uint8_t kCapExpander = 0x43;
    if (answers(M5.In_I2C, kCapExpander)) {
        cap_bus = "internal_g8_g9";
        return;
    }
    if (!M5.Ex_I2C.isEnabled()) M5.Ex_I2C.begin();
    cap_bus = answers(M5.Ex_I2C, kCapExpander) ? "grove_g2_g1" : "none";
}

String status_json(bool for_model) {
    cJSON* status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "runtime", "standalone");
    cJSON_AddStringToObject(status, "board", board_name().c_str());
    cJSON_AddStringToObject(status, "cap_io_expander", cap_bus.c_str());
    cJSON_AddStringToObject(status, "storage", storage::kind().c_str());
    cJSON_AddBoolToObject(status, "degraded", storage::degraded());
    cJSON_AddStringToObject(status, "sd_slow", storage::slow_report().c_str());
    cJSON_AddStringToObject(status, "i2c_internal", i2c_internal_devices.c_str());
    cJSON_AddNumberToObject(status, "storage_used_kb", static_cast<double>(storage::used_bytes() / 1024));
    cJSON_AddNumberToObject(status, "storage_total_kb", static_cast<double>(storage::total_bytes() / 1024));
    const uint64_t used = storage::used_bytes();
    if (!used_baseline_ms) {
        used_baseline = used;
        used_baseline_ms = millis();
    }
    const double hours = (millis() - used_baseline_ms) / 3600000.0;
    cJSON_AddNumberToObject(status, "storage_growth_kb", (static_cast<double>(used) - used_baseline) / 1024);
    cJSON_AddNumberToObject(status, "storage_growth_kb_per_h",
                            hours > 0.05 ? static_cast<long>((static_cast<double>(used) - used_baseline) / 1024 / hours) : 0);
    cJSON_AddStringToObject(status, "cpu_idle", cpu_text(idle_percent).c_str());
    cJSON_AddStringToObject(status, "cpu_idle_min", cpu_text(idle_percent_min).c_str());
    cJSON_AddStringToObject(status, "wifi", net::ssid().c_str());
    // The model's copy leaves out what identifies the network: it goes to the model provider.
    if (!for_model) cJSON_AddStringToObject(status, "ip", net::ip().c_str());
    cJSON_AddNumberToObject(status, "wifi_rssi", net::rssi());
    if (!for_model) cJSON_AddStringToObject(status, "wifi_bssid", net::bssid().c_str());
    cJSON_AddNumberToObject(status, "wifi_channel", net::channel());
    cJSON_AddNumberToObject(status, "free_heap", ESP.getFreeHeap());
    cJSON_AddNumberToObject(status, "min_free_heap", ESP.getMinFreeHeap());
    cJSON_AddNumberToObject(status, "max_block", ESP.getMaxAllocHeap());
    cJSON_AddNumberToObject(status, "open_sockets", open_sockets());
    cJSON_AddNumberToObject(status, "max_sockets", CONFIG_LWIP_MAX_SOCKETS);
    cJSON_AddNumberToObject(status, "worker_stack_free_min", worker::stack_free_min());
    cJSON_AddNumberToObject(status, "loop_stack_free_min", uxTaskGetStackHighWaterMark(nullptr));
    cJSON_AddNumberToObject(status, "battery_percent", M5Cardputer.Power.getBatteryLevel());
    cJSON_AddNumberToObject(status, "sd_mhz", storage::card_hz() / 1000000);
    cJSON_AddNumberToObject(status, "battery_mv", hw::battery_mv());
    cJSON_AddBoolToObject(status, "usb_host", hw::usb_connected());
    cJSON_AddStringToObject(status, "orientation", hw::imu_text().c_str());
    cJSON_AddNumberToObject(status, "uptime_s", millis() / 1000);
    cJSON_AddStringToObject(status, "reset_reason", reset_reason().c_str());
    const String json = json_print(status);
    cJSON_Delete(status);
    return json;
}

String settings_json() {
    const auto& cfg = config::get();
    return "{\"brightness\":" + String(cfg.brightness) +
           ",\"screen_timeout_seconds\":" + String(cfg.screen_timeout_seconds) +
           ",\"volume\":" + String(cfg.volume) + ",\"context_kb\":" + String(cfg.context_kb) + "}";
}

String resources_json() {
    return "[\"device://local/status\",\"device://local/settings\",\"device://local/battery\","
           "\"device://local/display/brightness\",\"device://local/display/timeout\","
           "\"device://local/display/power\",\"device://local/audio/volume\",\"device://local/context/length\","
           "\"device://local/clipboard\",\"memory://recent-tasks\",\"memory://notes\"]";
}

String clipboard() { return storage::read_text("/vat/memory/clipboard.txt", 2000); }

String tool_read(const cJSON* arguments) {
    const String target = json_str(arguments, "target");
    if (target == "device://local/status") return status_json(true);
    if (target == "device://local/settings") return settings_json();
    if (target == "device://local/battery") {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "percent", M5Cardputer.Power.getBatteryLevel());
        cJSON_AddNumberToObject(data, "voltage", hw::battery_mv() / 1000.0);
        cJSON_AddBoolToObject(data, "usb", hw::usb_connected());
        registry::offer_data(data);
        return hw::battery_text();
    }
    if (target == "device://local/display/brightness") return "亮度 " + String(config::get().brightness);
    if (target == "device://local/display/timeout") {
        return "息屏时间 " + String(config::get().screen_timeout_seconds) + " 秒";
    }
    if (target == "device://local/audio/volume") return "音量 " + String(config::get().volume) + "%";
    if (target == "device://local/context/length") {
        return "对话记忆 " + String(config::get().context_kb) + " KB（短 32 / 中 64 / 长 96；越长越慢、越费 token）";
    }
    if (target == "device://local/clipboard") {
        const String value = clipboard();
        return value.isEmpty() ? String("剪贴板为空") : value;
    }
    if (target == "memory://recent-tasks") {
        const String tail = context::recent_operations(3000);
        return tail.isEmpty() ? String("暂无历史操作") : tail;
    }
    if (target == "memory://notes") {
        const String notes = storage::read_tail("/vat/memory/notes.txt", 3000);
        return notes.isEmpty() ? String("暂无记忆笔记") : notes;
    }
    return "本机没有该资源: " + target;
}

String tool_write(const cJSON* arguments) {
    const String target = json_str(arguments, "target");
    const String content = json_str(arguments, "content");
    auto& cfg = config::get();
    if (target == "device://local/display/brightness") {
        const int value = constrain(content.toInt(), 8, 255);
        cfg.brightness = value;
        ui::post_brightness(value);
        return config::save_ui_number("brightness", value) ? "屏幕亮度已设置为 " + String(value)
                                                            : String("亮度生效但保存失败");
    }
    if (target == "device://local/context/length") {
        String v = content;
        v.trim();
        const int asked = v == "短" ? 32 : v == "中" ? 64 : v == "长" ? 96 : static_cast<int>(v.toInt());
        const int value = asked <= 0 ? 64 : asked < 48 ? 32 : asked < 80 ? 64 : 96;
        cfg.context_kb = value;
        return config::save_ui_number("context_kb", value)
                   ? "对话记忆已设为 " + String(value) + " KB" + (value == 96 ? "（每轮会慢一些、更费 token）" : "")
                   : String("设置生效但保存失败");
    }
    if (target == "device://local/display/timeout") {
        const int value = constrain(content.toInt(), 0, 1800);
        cfg.screen_timeout_seconds = value;
        return config::save_ui_number("screen_timeout_seconds", value)
                   ? "息屏时间已设置为 " + String(value) + " 秒"
                   : String("设置生效但保存失败");
    }
    if (target == "device://local/display/power") {
        String value = content;
        value.toLowerCase();
        const bool on = value == "on" || value == "1" || value == "true" || value == "打开";
        ui::post_screen_power(on);
        return on ? String("屏幕已打开") : String("屏幕已关闭，按任意键唤醒");
    }
    if (target == "device://local/audio/volume") {
        const int value = constrain(content.toInt(), 0, 100);
        cfg.volume = value;
        ui::post_beep();
        return config::save_ui_number("volume", value) ? "音量已设置为 " + String(value) + "%"
                                                        : String("音量生效但保存失败");
    }
    if (target == "device://local/clipboard") {
        return storage::write_text("/vat/memory/clipboard.txt", content)
                   ? "已复制到本机剪贴板，SSH 页 Fn+V 可粘贴到草稿"
                   : String("剪贴板写入失败");
    }
    if (target == "memory://notes") {
        return storage::append_line("/vat/memory/notes.txt", content) ? "已记住：" + content
                                                                       : String("记忆写入失败");
    }
    return "本机资源不可写入: " + target;
}


void sample_cpu() {
    const unsigned long now = millis();
    if (cpu_window_start && now - cpu_window_start < kCpuWindowMs) return;
    const uint32_t wall = static_cast<uint32_t>(esp_timer_get_time());
    for (int core = 0; core < 2; ++core) {
        const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
        if (cpu_window_start && wall != wall_before) {
            idle_percent[core] = std::min<int>(100, static_cast<uint64_t>(idle - idle_before[core]) * 100 / (wall - wall_before));
            idle_percent_min[core] = std::min(idle_percent_min[core], idle_percent[core]);
        }
        idle_before[core] = idle;
    }
    wall_before = wall;
    cpu_window_start = now;
}

}  // namespace vat::local
