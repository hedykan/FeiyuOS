#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

#include "ui_state.h"

// Full-screen overlay (display package: big text, QR code, image; timers):
// drawn over the current page until any key or the side button closes it.
// The page underneath is untouched; the caller redraws it after dismiss().
namespace vat::ui::overlay {

bool open();
const char* name();  // "text" | "qr" | "image", "" when closed
void show(Overlay incoming);
void draw(bool force);  // only when new, or on a forced redraw (images are drawn once)
void dismiss();
bool is_panel();
// A key while a panel is open: ; . move, Enter chooses (records the choice and
// emits ui://panel/<name> with {"choice", "index"}), ` closes.  true: close it.
bool panel_key(const Keyboard_Class::KeysState& state);

}  // namespace vat::ui::overlay
