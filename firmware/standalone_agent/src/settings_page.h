#pragma once

#include <M5Cardputer.h>
#include <cJSON.h>

#include <functional>

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

// For the settings package (other tasks): `work` runs on the UI task at its
// next poll(); its result, or an error after `wait_ms`.
String on_ui(std::function<String()> work, uint32_t wait_ms = 2000);
void poll();  // UI task, every loop
// UI task: every menu row as "名称：值" lines (secrets as hints only).
String rows();
// UI task: opens the settings page at `page` (a view name: wifi, keys, statusbar,
// animation…, or key/<provider> for the masked Key form); the result for the agent.
String show_page(const String& page);

}  // namespace vat::ui::settings
