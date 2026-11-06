#pragma once

#include "common.h"

// The saved Wi-Fi networks in secrets.json ("wifi": {ssid, password, networks:
// [{name, ssid, password}]}), as pure JSON edits so they also run on the host.
namespace vat::wifi_saved {

// `ssid` joined successfully: it becomes the primary network (tried first at
// boot) and is listed with its password.  The primary it replaces stays in the
// list: setups that wrote only the primary had no other copy of that password,
// and joining a phone hotspot once used to make the device forget home.
void remember(cJSON* wifi, const String& ssid, const String& password);

}  // namespace vat::wifi_saved
