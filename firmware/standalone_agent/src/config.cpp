#include "config.h"

#include "common.h"
#include "storage.h"

#include <functional>

namespace vat::config {
namespace {

Config current;

// The persona is appended raw into the quoted JSON system prompt, so it must
// carry no " \ or control characters; it is also length-capped.  (Flavor only
// — reply length and formatting stay in the core prompt.)
String sanitize_persona(const String& text) {
    String out;
    for (std::size_t i = 0; i < text.length() && out.length() < 400; ++i) {
        const char c = text[i];
        if (c == '"') out += '\'';
        else if (c == '\\') out += '/';
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return utf8_head(out, 360);  // trim on a UTF-8 boundary
}

cJSON* parse_file(const char* path) {
    const String text = storage::read_text(path, 32768);
    if (text.isEmpty()) return nullptr;
    return cJSON_Parse(text.c_str());
}

bool update_json_file(const char* path, const char* section,
                      const std::function<void(cJSON*)>& mutate) {
    cJSON* root = parse_file(path);
    if (!root) root = cJSON_CreateObject();
    cJSON* object = json_obj(root, section);
    if (!object) {
        object = cJSON_CreateObject();
        cJSON_AddItemToObject(root, section, object);
    }
    mutate(object);
    char* printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) return false;
    const bool ok = storage::write_text(path, String(printed));
    cJSON_free(printed);
    return ok;
}

void replace_item(cJSON* object, const char* key, cJSON* value) {
    if (cJSON_HasObjectItem(object, key)) {
        cJSON_ReplaceItemInObjectCaseSensitive(object, key, value);
    } else {
        cJSON_AddItemToObject(object, key, value);
    }
}

}  // namespace

Config& get() { return current; }

String load() {
    Config next;
    String problem;

    if (cJSON* config = parse_file("/vat/config.json")) {
        if (cJSON* stt = json_obj(config, "stt")) {
            next.stt_provider = json_str(stt, "provider", next.stt_provider);
            next.stt_model = json_str(stt, "model", next.stt_model);
            next.stt_url = json_str(stt, "url", next.stt_url);
            next.stt_timeout_ms = json_int(stt, "timeout_ms", next.stt_timeout_ms);
        }
        if (cJSON* agent = json_obj(config, "agent")) {
            next.agent_url = json_str(agent, "url", next.agent_url);
            next.agent_model = json_str(agent, "model", next.agent_model);
            next.agent_round_timeout_ms = json_int(agent, "timeout_ms", next.agent_round_timeout_ms);
            next.agent_persona = sanitize_persona(json_str(agent, "persona", next.agent_persona));
            next.agent_total_timeout_ms = json_int(agent, "total_timeout_ms", next.agent_total_timeout_ms);
        }
        if (cJSON* ui = json_obj(config, "ui")) {
            next.brightness = constrain(json_int(ui, "brightness", next.brightness), 8, 255);
            next.screen_timeout_seconds =
                constrain(json_int(ui, "screen_timeout_seconds", next.screen_timeout_seconds), 0, 1800);
            next.volume = constrain(json_int(ui, "volume", next.volume), 0, 100);
            const long font = json_int(ui, "terminal_font", next.terminal_font);
            next.terminal_font = font == 10 || font == 14 ? font : 12;
        }
        if (cJSON* gps = json_obj(config, "gps")) next.gps_enabled = json_bool(gps, "enabled", true);
        if (cJSON* lora = json_obj(config, "lora")) {
            auto number = [lora](const char* key, double fallback) {
                const cJSON* item = cJSON_GetObjectItemCaseSensitive(lora, key);
                return cJSON_IsNumber(item) ? item->valuedouble : fallback;
            };
            next.lora.enabled = json_bool(lora, "enabled", false);
            next.lora.frequency_mhz = number("frequency_mhz", 0);
            next.lora.power_dbm = constrain(static_cast<int>(number("power_dbm", next.lora.power_dbm)), -9, 22);
            next.lora.bandwidth_khz = number("bandwidth_khz", next.lora.bandwidth_khz);
            next.lora.spreading_factor = constrain(static_cast<int>(number("spreading_factor", 9)), 5, 12);
            next.lora.coding_rate = constrain(static_cast<int>(number("coding_rate", 7)), 5, 8);
            next.lora.antenna_confirmed = json_bool(lora, "antenna_confirmed", false);
        }
        next.default_profile = json_str(config, "default_ssh_profile", "");
        cJSON_Delete(config);
    }

    if (cJSON* secrets = parse_file("/vat/secrets.json")) {
        if (cJSON* wifi = json_obj(secrets, "wifi")) {
            next.wifi_ssid = json_str(wifi, "ssid");
            next.wifi_password = json_str(wifi, "password");
            cJSON* networks = cJSON_GetObjectItemCaseSensitive(wifi, "networks");
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, networks) {
                next.wifi_networks.push_back(
                    {json_str(item, "name"), json_str(item, "ssid"), json_str(item, "password")});
            }
        }
        if (cJSON* providers = json_obj(secrets, "providers")) {
            next.qwen_key = json_str(json_obj(providers, "qwen"), "api_key");
            next.openrouter_key = json_str(json_obj(providers, "openrouter"), "api_key");
            next.deepseek_key = json_str(json_obj(providers, "deepseek"), "api_key");
            next.amap_key = json_str(json_obj(providers, "amap"), "api_key");
        }
        cJSON_Delete(secrets);
    }

    if (cJSON* profiles = parse_file("/vat/ssh/profiles.json")) {
        cJSON* list = cJSON_GetObjectItemCaseSensitive(profiles, "profiles");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, list) {
            SshProfile profile;
            profile.id = json_str(item, "id");
            profile.host = json_str(item, "host");
            profile.port = json_int(item, "port", 22);
            profile.user = json_str(item, "user");
            profile.project_root = json_str(item, "project_root");
            cJSON* auth = json_obj(item, "auth");
            profile.password = json_str(auth, "password");
            profile.key_path = json_str(auth, "private_key");
            if (!profile.id.isEmpty() && !profile.host.isEmpty()) {
                next.profiles.push_back(profile);
            }
        }
        cJSON_Delete(profiles);
    }
    if (next.default_profile.isEmpty() && !next.profiles.empty()) {
        next.default_profile = next.profiles.front().id;
    }

    if (next.wifi_ssid.isEmpty()) {
        problem = "缺少 Wi-Fi 配置";
    } else if ((next.stt_provider == "qwen" && next.qwen_key.isEmpty()) ||
               (next.stt_provider == "openrouter" && next.openrouter_key.isEmpty())) {
        problem = "缺少语音识别 Key";
    } else if (next.deepseek_key.isEmpty()) {
        problem = "缺少 Agent Key";
    }
    current = next;
    return problem;
}

bool network_ready() { return !current.wifi_ssid.isEmpty(); }

bool providers_ready() {
    const bool stt = current.stt_provider == "qwen" ? !current.qwen_key.isEmpty()
                                                    : !current.openrouter_key.isEmpty();
    return stt && !current.deepseek_key.isEmpty();
}

const SshProfile* find_profile(const String& id) {
    for (const auto& profile : current.profiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

bool save_value(const char* section, const char* key, cJSON* value) {
    return update_json_file("/vat/config.json", section, [&](cJSON* object) { replace_item(object, key, value); });
}

bool save_ui_number(const char* key, double value) { return save_value("ui", key, cJSON_CreateNumber(value)); }

String set_agent_persona(const String& text) {
    current.agent_persona = sanitize_persona(text);
    const bool ok = save_value("agent", "persona", cJSON_CreateString(current.agent_persona.c_str()));
    return ok ? String("已更新角色设定，下次对话生效") : String("保存失败");
}

String profile_names() {
    String names;
    for (const auto& profile : current.profiles) {
        if (!names.isEmpty()) names += ", ";
        names += profile.id;
    }
    return names;
}

}  // namespace vat::config
