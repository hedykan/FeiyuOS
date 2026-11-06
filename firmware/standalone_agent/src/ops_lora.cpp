#include "ops_lora.h"

#include <M5Cardputer.h>
#include <RadioLib.h>
#include <SPI.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include <atomic>
#include <deque>

#include "common.h"
#include "config.h"
#include "context.h"
#include "storage.h"
#include "ui_state.h"

namespace vat::lora {
namespace {

constexpr uint8_t kExpander = 0x43;        // PI4IOE5V6408: P0 powers the RF switch
constexpr uint32_t kI2cHz = 400000;
constexpr float kTcxoVolts = 1.8;          // the cap's TCXO (RadioLib's default 1.6 V fails)
constexpr std::size_t kMaxPayload = 200;
constexpr std::size_t kInboxKept = 20;
constexpr char kInbox[] = "/vat/things/lora/inbox.jsonl";

struct Message {
    String time;
    bool outgoing = false;
    String text;
    float rssi = 0;
    float snr = 0;
};

SX1262* radio = nullptr;
SemaphoreHandle_t radio_mutex = nullptr;  // UI task (receive) vs worker (send)
std::atomic<bool> packet_flag{false};
bool receiving = false;
String off_reason = "未开启";
std::deque<Message> inbox;
uint32_t received_count = 0;

struct Lock {
    bool held;
    explicit Lock(TickType_t wait) : held(radio_mutex && xSemaphoreTake(radio_mutex, wait) == pdTRUE) {}
    ~Lock() {
        if (held) xSemaphoreGive(radio_mutex);
    }
};

void IRAM_ATTR on_packet() { packet_flag = true; }

String clock_now() {
    struct tm now;
    if (!getLocalTime(&now, 0)) return "--:--";
    char text[6];
    snprintf(text, sizeof(text), "%02d:%02d", now.tm_hour, now.tm_min);
    return text;
}

// The bus the cap's expander answers on (the docs allow G8/G9 or G2/G1).
m5::I2C_Class* expander_bus() {
    if (M5.In_I2C.isEnabled() && M5.In_I2C.scanID(kExpander, 100000)) return &M5.In_I2C;
    if (!M5.Ex_I2C.isEnabled()) M5.Ex_I2C.begin();
    if (M5.Ex_I2C.isEnabled() && M5.Ex_I2C.scanID(kExpander, 100000)) return &M5.Ex_I2C;
    return nullptr;
}

bool rf_switch(bool on) {
    m5::I2C_Class* bus = expander_bus();
    if (!bus) return false;
    // Direction output, not high-Z, then drive P0 (see the cap notes).
    return bus->writeRegister8(kExpander, 0x03, 0x01, kI2cHz) && bus->writeRegister8(kExpander, 0x07, 0x00, kI2cHz) &&
           bus->writeRegister8(kExpander, 0x05, on ? 0x01 : 0x00, kI2cHz);
}

void remember(const Message& message) {
    inbox.push_back(message);
    while (inbox.size() > kInboxKept) inbox.pop_front();
    if (!context::on_card()) return;
    cJSON* record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "time", message.time.c_str());
    cJSON_AddBoolToObject(record, "outgoing", message.outgoing);
    cJSON_AddStringToObject(record, "text", message.text.c_str());
    cJSON_AddNumberToObject(record, "rssi", message.rssi);
    cJSON_AddNumberToObject(record, "snr", message.snr);
    storage::append_line(kInbox, json_print(record));
    cJSON_Delete(record);
}

void load_inbox() {
    if (!context::on_card()) return;
    const String tail = storage::read_tail(kInbox, 6000);
    int start = 0;
    while (start < static_cast<int>(tail.length())) {
        int end = tail.indexOf('\n', start);
        if (end < 0) end = tail.length();
        cJSON* record = cJSON_Parse(tail.substring(start, end).c_str());
        start = end + 1;
        if (!record) continue;
        Message message;
        message.time = json_str(record, "time");
        message.outgoing = json_bool(record, "outgoing", false);
        message.text = json_str(record, "text");
        const cJSON* rssi = cJSON_GetObjectItemCaseSensitive(record, "rssi");
        const cJSON* snr = cJSON_GetObjectItemCaseSensitive(record, "snr");
        message.rssi = cJSON_IsNumber(rssi) ? rssi->valuedouble : 0;
        message.snr = cJSON_IsNumber(snr) ? snr->valuedouble : 0;
        inbox.push_back(message);
        cJSON_Delete(record);
    }
    while (inbox.size() > kInboxKept) inbox.pop_front();
}

void stop_radio() {
    if (radio) {
        Lock lock(pdMS_TO_TICKS(2000));
        radio->sleep();
    }
    // Only switch the RF path off if we switched it on: 0x43 still answered
    // with the cap removed, so it is not certain the expander is the cap's.
    if (receiving) rf_switch(false);
    receiving = false;
    hold_off_bus();
}

bool frequency_ok(float mhz) { return mhz >= 868.0f && mhz <= 923.0f; }

}  // namespace

void hold_off_bus() {
    pinMode(kNss, OUTPUT);
    digitalWrite(kNss, HIGH);
    pinMode(kReset, OUTPUT);
    digitalWrite(kReset, LOW);  // in reset every SX1262 pin is high-Z
}

bool cap_present() { return expander_bus() != nullptr; }

String begin() {
    if (!radio_mutex) radio_mutex = xSemaphoreCreateMutex();
    if (inbox.empty()) load_inbox();
    const auto& cfg = config::get().lora;
    off_reason = "";
    if (!cap_present()) off_reason = "未检测到 Cap LoRa-1262";
    else if (!cfg.enabled) off_reason = "未开启";
    else if (!frequency_ok(cfg.frequency_mhz)) off_reason = "未设置频点（868–923 MHz）";
    if (!off_reason.isEmpty()) {
        stop_radio();
        return off_reason;
    }
    if (!rf_switch(true)) {
        off_reason = "射频开关（0x43）写入失败";
        stop_radio();
        return off_reason;
    }
    Lock lock(pdMS_TO_TICKS(2000));
    if (!radio) radio = new SX1262(new Module(kNss, kIrq, kReset, kBusy, SPI, SPISettings(8000000, MSBFIRST, SPI_MODE0)));
    int state = radio->begin(cfg.frequency_mhz, cfg.bandwidth_khz, cfg.spreading_factor, cfg.coding_rate,
                             RADIOLIB_SX126X_SYNC_WORD_PRIVATE, cfg.power_dbm, 8, kTcxoVolts, false);
    if (state == RADIOLIB_ERR_NONE) state = radio->setDio2AsRfSwitch(true);
    if (state == RADIOLIB_ERR_NONE) {
        radio->setPacketReceivedAction(on_packet);
        state = radio->startReceive();
    }
    if (state != RADIOLIB_ERR_NONE) {
        off_reason = "SX1262 初始化失败（错误 " + String(state) + "）";
        receiving = false;
        runtime_event("lora", off_reason);
        return off_reason;
    }
    receiving = true;
    runtime_event("lora", "receiving " + String(cfg.frequency_mhz, 3) + " MHz SF" + String(cfg.spreading_factor));
    return "";
}

void loop() {
    if (!receiving || !packet_flag) return;
    Lock lock(0);  // a send in progress: try again on the next loop
    if (!lock.held) return;
    packet_flag = false;
    const std::size_t length = radio->getPacketLength();
    uint8_t buffer[256];
    const int state = radio->readData(buffer, std::min<std::size_t>(length, sizeof(buffer)));
    Message message;
    message.time = clock_now();
    message.rssi = radio->getRSSI();
    message.snr = radio->getSNR();
    radio->startReceive();
    if (state != RADIOLIB_ERR_NONE || length == 0) return;
    message.text = utf8_sanitize(String(reinterpret_cast<const char*>(buffer), std::min<std::size_t>(length, sizeof(buffer))));
    ++received_count;
    remember(message);
    ui::set_notice("LoRa：" + utf8_head(message.text, 60));
    runtime_event("lora_rx", String(length) + " bytes rssi=" + String(message.rssi, 0));
}

String status_text() {
    const auto& cfg = config::get().lora;
    String text = receiving ? String("LoRa：接收中") : "LoRa：关闭（" + off_reason + "）";
    text += "\n频点 " + (cfg.frequency_mhz > 0 ? String(cfg.frequency_mhz, 3) + " MHz" : String("未设置")) +
            "，功率 " + String(cfg.power_dbm) + " dBm，SF" + String(cfg.spreading_factor) + " BW" +
            String(cfg.bandwidth_khz, 1) + " CR4/" + String(cfg.coding_rate);
    text += "\n天线：" + String(cfg.antenna_confirmed ? "已确认装好" : "未确认（确认前不会发射）");
    text += "\n本次开机收到 " + String(received_count) + " 条";
    if (!inbox.empty()) {
        const auto& last = inbox.back();
        text += "，最近一条 " + last.time;
        text += last.outgoing ? String("（发出）") : " RSSI " + String(last.rssi, 0) + " dBm SNR " + String(last.snr, 1);
    }
    return text;
}

String messages_text() {
    if (inbox.empty()) return "还没有 LoRa 消息";
    String text = "LoRa 消息（新的在后）：\n";
    int n = 0;
    for (const auto& message : inbox) {
        text += String(++n) + ". " + message.time + (message.outgoing ? " 发出：" : " 收到：") + message.text;
        if (!message.outgoing) text += "（" + String(message.rssi, 0) + " dBm）";
        text += "\n";
    }
    return text;
}

String send(const String& text) {
    const auto& cfg = config::get().lora;
    if (!receiving) return "LoRa 未开启：" + off_reason;
    if (!cfg.antenna_confirmed) return "未确认天线已装好，不发射（没装天线发射会损坏功放）。确认后说“LoRa 天线已经装好”。";
    if (text.isEmpty()) return "消息为空";
    if (text.length() > kMaxPayload) return "消息太长：最多 " + String(kMaxPayload) + " 字节，当前 " + String(text.length());
    Lock lock(pdMS_TO_TICKS(3000));
    if (!lock.held) return "LoRa 忙，稍后再试";
    const unsigned long started = millis();
    const int state = radio->transmit(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
    radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) return "发送失败（错误 " + String(state) + "）";
    Message message;
    message.time = clock_now();
    message.outgoing = true;
    message.text = text;
    remember(message);
    return "已发送 " + String(text.length()) + " 字节，用时 " + String(millis() - started) + " ms";
}

String set(const String& key, const String& raw) {
    auto& cfg = config::get().lora;
    String value = raw;
    value.trim();
    bool ok = false;
    if (key == "enabled") {
        const bool on = value == "on" || value == "true" || value == "1" || value.indexOf("开") >= 0;
        return enable(on);
    }
    if (key == "frequency") {
        const float mhz = value.toFloat();
        if (!frequency_ok(mhz)) return "频点需在 868–923 MHz（这块 Cap 的范围；请按当地规定选择）";
        cfg.frequency_mhz = mhz;
        ok = config::save_value("lora", "frequency_mhz", cJSON_CreateNumber(mhz));
    } else if (key == "power") {
        const int dbm = value.toInt();
        if (dbm < -9 || dbm > 22) return "功率需在 -9–22 dBm";
        cfg.power_dbm = dbm;
        ok = config::save_value("lora", "power_dbm", cJSON_CreateNumber(dbm));
    } else if (key == "sf") {
        const int sf = value.toInt();
        if (sf < 5 || sf > 12) return "扩频因子需在 5–12";
        cfg.spreading_factor = sf;
        ok = config::save_value("lora", "spreading_factor", cJSON_CreateNumber(sf));
    } else if (key == "bandwidth") {
        const float khz = value.toFloat();
        static const float allowed[] = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500};
        bool valid = false;
        for (const float a : allowed) valid = valid || fabsf(a - khz) < 0.01f;
        if (!valid) return "带宽只能是 7.8/10.4/15.6/20.8/31.25/41.7/62.5/125/250/500 kHz";
        cfg.bandwidth_khz = khz;
        ok = config::save_value("lora", "bandwidth_khz", cJSON_CreateNumber(khz));
    } else if (key == "cr") {
        const int cr = value.toInt();
        if (cr < 5 || cr > 8) return "编码率需在 5–8（即 4/5–4/8）";
        cfg.coding_rate = cr;
        ok = config::save_value("lora", "coding_rate", cJSON_CreateNumber(cr));
    } else if (key == "antenna") {
        const bool installed = value == "installed" || value == "on" || value == "true" || value.indexOf("装好") >= 0;
        cfg.antenna_confirmed = installed;
        ok = config::save_value("lora", "antenna_confirmed", cJSON_CreateBool(installed));
    } else {
        return "可设置：enabled（on/off）、frequency、power、sf、bandwidth、cr、antenna";
    }
    if (!ok) return "保存失败";
    if (receiving || key == "frequency") {
        const String problem = begin();  // apply the new radio settings
        if (!problem.isEmpty() && cfg.enabled) return "已保存，但未能开始接收：" + problem;
    }
    return "已保存。" + status_text();
}

String enable(bool on) {
    auto& cfg = config::get().lora;
    cfg.enabled = on;
    if (!config::save_value("lora", "enabled", cJSON_CreateBool(on))) return "保存失败";
    if (!on) {
        stop_radio();
        off_reason = "未开启";
        return "LoRa 已关闭（芯片保持复位，不占用总线）";
    }
    const String problem = begin();
    return problem.isEmpty() ? "LoRa 已开启，正在接收。\n" + status_text() : "LoRa 未能开启：" + problem;
}

}  // namespace vat::lora
