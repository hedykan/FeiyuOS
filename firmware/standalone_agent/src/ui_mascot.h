#pragma once

#include "ui_kit.h"

// Corner mascot: a 32×32 pixel character in the bottom-right corner of the
// Agent list that follows the newest task (idle / recognising / thinking /
// running / failed).  The art is not in the firmware: frames are read from
// /vat/assets/mascot.bin (made by tools/pack_mascot.py), one 1 KB
// frame at a time into a static buffer.  Without the file nothing is drawn
// and the page keeps its full width.
//
// Reading the card and drawing are separate calls: load() must run outside
// any display transaction, push() inside one.
namespace vat::ui::mascot {

constexpr int kSize = 32;
constexpr int kLeft = kWidth - kSize;
constexpr int kTop = kHeight - kSize;

enum class State : uint8_t { Idle, Recognize, Understand, Execute, Error };

bool enabled();             // file present and valid; retried every 30 s while missing
void request(State state);  // what the task is doing; a change restarts the loop
bool due();                 // advance to the next frame once its time has come
bool load();                // read the current frame from the card
void push(Gfx& g);          // draw the loaded frame; transparent pixels take the backdrop
// What the page has behind the mascot, per screen row (a selected row is
// highlighted): transparent pixels are drawn in that colour, row by row, so
// the mascot looks see-through without clearing and redrawing (no flicker).
void reset_backdrop(uint16_t color);
void set_backdrop(int screen_y, int height, uint16_t color);
const char* describe();     // shown state name, "off" without the file

}  // namespace vat::ui::mascot
