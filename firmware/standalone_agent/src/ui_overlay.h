#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

#include "ui_state.h"

// The graphics page's picture (display package: big text, QR code, image;
// ui panels and canvases).  ui.cpp shows it on the graphics page only; any
// key closes it (a panel: its own keys), Btn0 leaves it open.  Only a
// description is kept between draws: leaving the page drops the animation's
// frame buffer (suspend), coming back draws it again.
namespace vat::ui::overlay {

bool open();
const char* name();  // "text" | "qr" | "image", "" when closed
// The canvas on screen: frames drawn so far and why its update stopped ("" when running).
// Safe to read from the web task.
uint32_t canvas_frame();
const char* canvas_error();
// true when it is a new picture (not a refresh of the panel / canvas on screen).
bool show(Overlay incoming);
void suspend();  // the page was left: give back the animation frame
void draw(bool force);  // only when new, or on a forced redraw (images are drawn once)
void dismiss();
bool is_panel();
// A key while a panel is open: ; . move, Enter chooses (records the choice and
// emits ui://panel/<name> with {"choice", "index"}), ` closes.  true: close it.
bool panel_key(const Keyboard_Class::KeysState& state);

}  // namespace vat::ui::overlay
