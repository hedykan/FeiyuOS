#pragma once

#include <Arduino.h>

// Persistence behind the settings page: provider keys in /vat/secrets.json
// and SSH hosts in /vat/ssh/profiles.json.  Each call reloads the config.
namespace vat::settings_store {

String key_hint(const String& key);  // "未设置" or "已设置 …abcd"
// provider: qwen | deepseek | openrouter
String set_provider_key(const String& provider, const String& key);

// Named secrets of the http package ({secret:名字} in a header): the value and
// the one host it may be sent to, both fixed when it is typed on the device.
bool http_secret(const String& name, String& value, String& host);
// "" when saved, else why not.
String set_http_secret(const String& name, const String& host, const String& value);

// New hosts log in with the device key; the host still has to trust it.
String add_profile(const String& id, const String& host, const String& user, int port);
String remove_profile(const String& id);

// SSH page font size 10 | 12 | 14: saved and applied; "" or the reason it was refused.
String set_terminal_font(int size);
// ui.animation 0 off, 1 reduced, 2 on.
String set_animation(int level);

}  // namespace vat::settings_store
