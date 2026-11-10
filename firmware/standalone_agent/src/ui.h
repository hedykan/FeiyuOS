#pragma once

#include <Arduino.h>

#include "ui_state.h"

namespace vat::ui {

void begin();
void loop();
void enter_setup(const String& reason);
void show_ready();  // READY page with current network/profile info

// Actions shared by the keyboard and the device web API.
String action(const String& name);
Page page();
String screen_text();  // plain-text model of what is on screen
String press(const String& spec);  // a key press from the web API (tests)

}  // namespace vat::ui
