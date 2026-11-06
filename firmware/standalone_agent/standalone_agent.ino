// Voice Agent Terminal — standalone Cardputer firmware.
//
// Everything runs on the device: push-to-talk recording, speech recognition,
// the seven-tool operation agent, SSH (libssh), remote tmux/Codex/Pi, the
// Agent/SSH/History UI and an on-device web admin page.  USB is only used to
// flash the firmware.

#include <M5Cardputer.h>
#include <esp_system.h>

#include "src/events.h"
#include "src/agent.h"
#include "src/common.h"
#include "src/config.h"
#include "src/context.h"
#include "src/net.h"
#include "src/ops_gps.h"
#include "src/ops_lora.h"
#include "src/ops_wifi.h"
#include "src/ops_local.h"
#include "src/ops_ssh.h"
#include "src/ops_portal.h"
#include "src/ops_timer.h"
#include "src/registry.h"
#include "src/storage.h"
#include "src/ui.h"
#include "src/web.h"
#include "src/worker.h"

// UI loop task (keyboard, drawing, web pages, settings): 8 KB left only 3.7 KB
// unused at the deepest point measured in v0.5.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    // Mic and speaker share I2S: neither runs at boot; recording starts the mic,
    // a beep starts the speaker briefly between recordings (ops_hw).
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(128);
    vat::local::probe_hardware();
    // The microphone is started only while recording (see ui.cpp record()).
    vat::ui::show_boot("BOOT", "正在挂载存储…", TFT_YELLOW);

    if (!vat::storage::begin()) {
        vat::ui::show_boot("STORAGE ERROR", "TF 与板载 Flash 均不可用", TFT_RED);
        return;
    }
    const String problem = vat::config::load();
    M5Cardputer.Display.setBrightness(vat::config::get().brightness);
    vat::runtime_event("boot", vat::storage::kind() + " " + (problem.isEmpty() ? String("config ok") : problem) +
                                   " reset=" + vat::reset_reason());

    vat::registry::register_builtin();
    vat::ui::begin();
    vat::context::begin();
    vat::agent::begin();
    const String lora_state = vat::lora::begin();  // stays off (radio in reset) unless enabled
    if (!lora_state.isEmpty()) vat::runtime_event("lora", lora_state);
    const String gps_state = vat::gps::begin();
    if (!gps_state.isEmpty()) vat::runtime_event("gps", gps_state);
    vat::worker::begin();

    bool online = false;
    if (vat::config::network_ready()) {
        vat::ui::show_boot("WIFI", "正在连接 " + vat::config::get().wifi_ssid + "…", TFT_YELLOW);
        online = vat::wifi::connect_at_boot(18000);  // primary, then the other saved networks
    }
    if (!online) {
        vat::net::start_setup_ap();
        vat::web::begin();
        vat::ui::enter_setup(vat::config::network_ready() ? "WiFi 连接失败" : "首次配置");
        return;
    }
    configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org");  // timeline clock
    vat::timer::begin();
    vat::ssh::begin();
    vat::web::begin();
    if (!problem.isEmpty()) {
        vat::ui::enter_setup(problem);
        return;
    }
    vat::ui::show_ready();
    if (vat::storage::degraded()) vat::ui::set_notice("TF 卡不可用：已用板载备份启动，长上下文暂停", true);
    vat::runtime_event("ready", vat::net::ip());
    vat::events::emit("device://boot");
}

void loop() {
    // Software watchdog for the worker (replaces the idle-task check).
    if (vat::worker::running_ms() > vat::worker::kStuckJobMs) {
        // No storage::flush() here: the stuck job may hold the card.
        Serial.printf("[watchdog] worker job stuck for %lu ms, restarting\n",
                      static_cast<unsigned long>(vat::worker::running_ms()));
        delay(200);
        esp_restart();
    }
    M5Cardputer.update();
    vat::web::loop();
    vat::portal::loop();
    vat::storage::tick();
    vat::local::sample_cpu();
    if (!vat::worker::busy()) {  // never remount under a running job
        const String card = vat::storage::check_card();
        if (card == "lost") {
            vat::ui::set_notice("TF 卡掉线，每 30 秒重试；重启可改用板载备份", true);
        } else if (card == "remounted") {
            vat::ui::set_notice("TF 卡掉线后已重新挂载");
            vat::runtime_event("sd_remount", String(vat::storage::card_hz() / 1000000) + " MHz");
        }
    }
    vat::lora::loop();
    vat::gps::loop();
    vat::ui::loop();
    delay(2);
}
