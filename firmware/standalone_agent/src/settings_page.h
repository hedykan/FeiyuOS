#pragma once

#include <M5Cardputer.h>
#include <cJSON.h>

// Settings page (Fn+W): WiFi, provider keys, SSH hosts and the device public
// key, LoRa, terminal font, storage.  Everything is typed on the device; no
// secret is shown in full or reported to the web API.
namespace vat::ui::settings {

// Offline at boot it starts at the WiFi list (setup hotspot on) and restarts
// once a network is joined; otherwise ` leaves it.
void open(const String& reason);
bool active();
void handle(const Keyboard_Class::KeysState& state);
void draw(bool force);        // when something changed; the menu every 5 s
void describe(cJSON* screen);  // for /api/status: typed text by length only

}  // namespace vat::ui::settings
