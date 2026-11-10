#pragma once

#include <Arduino.h>

#include <vector>

#include "ui_kit.h"

namespace vat::ui {

// Three bars for the Wi-Fi signal (12 x 9 px from x, y); red when offline.
void wifi_bars(Gfx& g, int x, int y);

// The status items on the right of the Agent page's top bar.  Which ones,
// and in what order, is a setting (ui.status_items; default time, battery,
// signal): Fn+W 顶栏, or settings://statusbar.
namespace status {

struct Item {
    const char* id;
    const char* label;
};
const std::vector<Item>& all();  // every item, in the default order
std::vector<String> chosen();    // the setting, unknown ids dropped
bool save(const std::vector<String>& ids);
String summary();  // "时间·电量·信号" (the settings menu)
// Draws the chosen items right to left, ending at `right` and never left of
// `left`: the first item that does not fit is left out with all after it.
void draw(Gfx& g, int top, int right, int left, uint16_t bg);
// True (at most once a second) when what draw() would show is not what it
// last showed: the minute, a battery step, the signal…  The bar is then
// redrawn on its own.
bool changed();

}  // namespace status
}  // namespace vat::ui
