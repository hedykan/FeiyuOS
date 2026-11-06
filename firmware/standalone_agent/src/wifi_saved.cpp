#include "wifi_saved.h"

namespace vat::wifi_saved {
namespace {

void set_string(cJSON* object, const char* key, const String& value) {
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddStringToObject(object, key, value.c_str());
}

// Drops every entry for `ssid` (by name or SSID).
void drop(cJSON* networks, const String& ssid) {
    for (int i = cJSON_GetArraySize(networks) - 1; i >= 0; --i) {
        const cJSON* item = cJSON_GetArrayItem(networks, i);
        if (json_str(item, "name") == ssid || json_str(item, "ssid") == ssid) cJSON_DeleteItemFromArray(networks, i);
    }
}

bool listed(const cJSON* networks, const String& ssid) {
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, networks) {
        if (json_str(item, "ssid") == ssid) return true;
    }
    return false;
}

void add(cJSON* networks, const String& ssid, const String& password) {
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", ssid.c_str());
    cJSON_AddStringToObject(entry, "ssid", ssid.c_str());
    cJSON_AddStringToObject(entry, "password", password.c_str());
    cJSON_AddItemToArray(networks, entry);
}

}  // namespace

void remember(cJSON* wifi, const String& ssid, const String& password) {
    cJSON* networks = cJSON_GetObjectItemCaseSensitive(wifi, "networks");
    if (!cJSON_IsArray(networks)) {
        cJSON_DeleteItemFromObjectCaseSensitive(wifi, "networks");
        networks = cJSON_AddArrayToObject(wifi, "networks");
    }
    const String old_ssid = json_str(wifi, "ssid");
    if (!old_ssid.isEmpty() && old_ssid != ssid && !listed(networks, old_ssid)) {
        add(networks, old_ssid, json_str(wifi, "password"));
    }
    drop(networks, ssid);
    add(networks, ssid, password);
    set_string(wifi, "ssid", ssid);
    set_string(wifi, "password", password);
}

}  // namespace vat::wifi_saved
