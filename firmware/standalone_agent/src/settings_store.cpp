#include "settings_store.h"

#include <cJSON.h>

#include "common.h"
#include "config.h"
#include "storage.h"
#include "ui_kit.h"

namespace vat::settings_store {
namespace {

constexpr char kSecrets[] = "/vat/secrets.json";
constexpr char kProfiles[] = "/vat/ssh/profiles.json";

bool write_json(const char* path, cJSON* root) {
    char* printed = cJSON_Print(root);
    const bool ok = printed && storage::write_text(path, String(printed));
    if (printed) cJSON_free(printed);
    if (ok) config::load();
    return ok;
}

cJSON* object_at(cJSON* parent, const char* key) {
    cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsObject(child)) return child;
    cJSON_DeleteItemFromObject(parent, key);
    return cJSON_AddObjectToObject(parent, key);
}

}  // namespace

String key_hint(const String& key) {
    if (key.isEmpty()) return "未设置";
    return "已设置 …" + key.substring(key.length() > 4 ? key.length() - 4 : 0);
}

String set_provider_key(const String& provider, const String& key_in) {
    String key = key_in;
    key.trim();
    if (key.isEmpty()) return "没有输入，未修改";
    if (key.length() < 16 || key.indexOf(' ') >= 0) return "看起来不像 Key（太短或含空格），未保存";
    cJSON* secrets = cJSON_Parse(storage::read_text(kSecrets, 32768).c_str());
    if (!secrets) secrets = cJSON_CreateObject();
    cJSON* entry = object_at(object_at(secrets, "providers"), provider.c_str());
    cJSON_DeleteItemFromObject(entry, "api_key");
    cJSON_AddStringToObject(entry, "api_key", key.c_str());
    const bool ok = write_json(kSecrets, secrets);
    cJSON_Delete(secrets);
    return ok ? String("已保存，下一次请求起生效（") + key_hint(key) + "）" : String("保存失败");
}

bool http_secret(const String& name, String& value, String& host) {
    String text = storage::read_text(kSecrets, 32768);
    cJSON* secrets = cJSON_Parse(text.c_str());
    wipe(text);
    const cJSON* entry = json_obj(json_obj(secrets, "http"), name.c_str());
    value = json_str(entry, "value");
    host = json_str(entry, "host");
    cJSON_Delete(secrets);
    return !value.isEmpty();
}

String set_http_secret(const String& name, const String& host, const String& value_in) {
    String value = value_in;
    value.trim();
    if (value.isEmpty()) return "没有输入，未保存";
    if (value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0) return "密钥不能换行，未保存";
    String text = storage::read_text(kSecrets, 32768);
    cJSON* secrets = cJSON_Parse(text.c_str());
    wipe(text);
    if (!secrets) secrets = cJSON_CreateObject();
    cJSON* entry = object_at(object_at(secrets, "http"), name.c_str());
    cJSON_DeleteItemFromObject(entry, "value");
    cJSON_DeleteItemFromObject(entry, "host");
    cJSON_AddStringToObject(entry, "value", value.c_str());
    cJSON_AddStringToObject(entry, "host", host.c_str());
    wipe(value);
    const bool ok = write_json(kSecrets, secrets);
    cJSON_Delete(secrets);
    return ok ? String() : String("保存失败");
}

String add_profile(const String& id_in, const String& host_in, const String& user_in, int port) {
    String id = id_in, host = host_in, user = user_in;
    id.trim();
    host.trim();
    user.trim();
    if (id.isEmpty() || host.isEmpty() || user.isEmpty()) return "名称、地址、用户名都要填";
    if (port <= 0 || port > 65535) return "端口不对";
    cJSON* root = cJSON_Parse(storage::read_text(kProfiles, 32768).c_str());
    if (!root) root = cJSON_CreateObject();
    cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(list)) {
        cJSON_DeleteItemFromObject(root, "profiles");
        list = cJSON_AddArrayToObject(root, "profiles");
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (json_str(item, "id") == id) {
            cJSON_Delete(root);
            return "已有同名主机「" + id + "」，先删除再添加";
        }
    }
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "id", id.c_str());
    cJSON_AddStringToObject(entry, "host", host.c_str());
    cJSON_AddNumberToObject(entry, "port", port);
    cJSON_AddStringToObject(entry, "user", user.c_str());
    cJSON* auth = cJSON_AddObjectToObject(entry, "auth");
    cJSON_AddStringToObject(auth, "type", "private_key");
    cJSON_AddStringToObject(auth, "private_key", "/vat/ssh/id_ed25519");
    cJSON_AddItemToArray(list, entry);
    const bool ok = write_json(kProfiles, root);
    cJSON_Delete(root);
    return ok ? "已添加「" + id + "」。还要让主机信任设备公钥：在 Agent 里说「给" + id +
                    "配置免密登录」并输入一次密码，或扫「设备公钥」二维码加到主机的 authorized_keys"
              : String("保存失败");
}

String remove_profile(const String& id) {
    cJSON* root = cJSON_Parse(storage::read_text(kProfiles, 32768).c_str());
    cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    bool removed = false;
    for (int i = cJSON_GetArraySize(list) - 1; i >= 0; --i) {
        if (json_str(cJSON_GetArrayItem(list, i), "id") == id) {
            cJSON_DeleteItemFromArray(list, i);
            removed = true;
        }
    }
    const bool ok = removed && write_json(kProfiles, root);
    cJSON_Delete(root);
    return !removed ? "没有找到「" + id + "」" : ok ? "已删除「" + id + "」" : String("保存失败");
}

String set_terminal_font(int size) {
    if (size != 10 && size != 12 && size != 14) return "字号只能是 10 / 12 / 14";
    config::get().terminal_font = size;
    config::save_ui_number("terminal_font", size);
    ui::apply_terminal_size();
    return {};
}

String set_animation(int level) {
    if (level < 0 || level > 2) return "动画只能是 开 / 减弱 / 关";
    config::get().animation = level;
    config::save_ui_number("animation", level);
    return {};
}

void set_reasoning(bool on) {
    config::get().reasoning = on;
    config::save_ui_number("reasoning", on ? 1 : 0);
}

}  // namespace vat::settings_store
