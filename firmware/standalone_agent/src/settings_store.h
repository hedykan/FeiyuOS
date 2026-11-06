#pragma once

#include <Arduino.h>

// Persistence behind the settings page: provider keys in /vat/secrets.json
// and SSH hosts in /vat/ssh/profiles.json.  Each call reloads the config.
namespace vat::settings_store {

String key_hint(const String& key);  // "未设置" or "已设置 …abcd"
// provider: qwen | deepseek | openrouter
String set_provider_key(const String& provider, const String& key);

// New hosts log in with the device key; the host still has to trust it.
String add_profile(const String& id, const String& host, const String& user, int port);
String remove_profile(const String& id);

// SSH page font size 10 | 12 | 14: saved and applied; "" or the reason it was refused.
String set_terminal_font(int size);

}  // namespace vat::settings_store
