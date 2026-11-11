#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <vector>

namespace vat {

struct WifiNetwork {
    String name;  // spoken name, e.g. "家里 WiFi"
    String ssid;
    String password;
};

struct SshProfile {
    String id;            // spoken/profile name, e.g. "开发机"
    String host;
    uint16_t port = 22;
    String user;
    String password;      // optional
    String key_path;      // optional, e.g. /vat/ssh/id_ed25519
    String project_root;  // where relative project names resolve
};

// Cap LoRa-1262.  Off until the user turns it on; transmitting additionally
// needs a frequency legal where the user is and a confirmed antenna.
struct LoraSettings {
    bool enabled = false;
    float frequency_mhz = 0;  // 0 = not set; the cap covers 868-923 MHz
    int power_dbm = 10;       // -9..22
    float bandwidth_khz = 125;
    int spreading_factor = 9; // 5..12
    int coding_rate = 7;      // 5..8 (4/5..4/8)
    bool antenna_confirmed = false;
};

// Bluetooth LE (ops_ble).  Off by default; read once at boot, so a change
// takes effect after a restart.
struct BleSettings {
    bool enabled = false;
};

struct Config {
    // network
    String wifi_ssid;
    String wifi_password;
    std::vector<WifiNetwork> wifi_networks;
    // providers
    String qwen_key;
    String openrouter_key;
    String deepseek_key;
    String amap_key;  // AMap (高德) web service key for the place package
    // speech
    String stt_provider = "qwen";
    String stt_model = "qwen3-asr-flash";
    String stt_url =
        "https://cn-beijing.maas.qianwenaiapi.com/api/v1/services/aigc/multimodal-generation/generation";
    uint32_t stt_timeout_ms = 25000;  // one transcription request
    // agent
    String agent_url = "https://api.deepseek.com/chat/completions";
    String agent_model = "deepseek-flash";
    // One model request.  A round may write a whole program (agent_max_tokens of
    // output), so this is far longer than a short answer needs.
    uint32_t agent_round_timeout_ms = 60000;  // at most 65 s: the HTTP client keeps it in 16 bits
    // Output tokens per round.  700 cut every program in the middle of its source;
    // 4000 holds about 10 KB of Lua (docs/V072_PLAN.zh-CN.md, T1).
    uint32_t agent_max_tokens = 4000;
    // Free-text personality appended to the system prompt (flavor only; editable
    // on the phone page).  Kept JSON-safe (no quote/backslash/control chars).
    String agent_persona = "你是终端里的大肥鱼——蓝发、傲娇又嘴甜的女仆鲸,聪明爱摸鱼、主食白饭、最忌被叫胖;自称「本小姐」,喊用户「主人」,嘴上傲娇、活照样办得利索。";
    // Model time per task (all rounds together).  Tool execution is not
    // counted: every tool has its own timeout, and waiting for the user is free.
    uint32_t agent_total_timeout_ms = 300000;
    // ui
    uint8_t brightness = 128;
    uint32_t screen_timeout_seconds = 60;
    uint8_t volume = 40;
    uint8_t terminal_font = 12;  // SSH page: 10 | 12 | 14
    // Conversation kept verbatim with a card, in KB (32 | 64 | 96); older
    // turns are summarised.  Longer costs tokens and time every round.
    uint8_t context_kb = 64;
    String status_items = "time,battery,signal";  // Agent page top bar (ui_status)
    uint8_t animation = 2;  // the settings cursor slide: 0 off, 1 reduced, 2 on
    // ssh
    std::vector<SshProfile> profiles;
    String default_profile;
    // extensions
    LoraSettings lora;
    BleSettings ble;
    bool gps_enabled = true;  // receive-only; reading the UART costs almost nothing
};

namespace config {

Config& get();
// Loads /vat/config.json, /vat/secrets.json and /vat/ssh/profiles.json.
// Missing files keep defaults; returns a human readable problem or "".
String load();
bool network_ready();  // Wi-Fi credentials present
bool providers_ready();  // STT + agent keys present
const SshProfile* find_profile(const String& id);
// Persist one value of a config.json section (takes ownership of `value`);
// the in-memory config is reloaded by the caller when needed.
bool save_value(const char* section, const char* key, cJSON* value);
bool save_ui_number(const char* key, double value);
// Set and persist the agent persona (sanitized); returns a status message.
String set_agent_persona(const String& text);
// Persist primary Wi-Fi credentials (Setup page).
String profile_names();  // "开发机, devbox"

}  // namespace config
}  // namespace vat
