#pragma once

// Theme, sizes and small text/keyboard helpers shared by the UI pages
// (ui.cpp and settings_page.cpp).
#include <M5Cardputer.h>

#include <vector>

namespace vat::ui {

using Gfx = lgfx::LovyanGFX;

// ---------------------------------------------------------------- theme
// Dark theme, one accent plus two semantic colours (RGB565).
constexpr uint16_t kBg = 0x0882;       // #0F1216
constexpr uint16_t kSurface = 0x18E4;  // #1A1F26
constexpr uint16_t kText = 0xE75E;     // #E6EAF0
constexpr uint16_t kMuted = 0x7C32;    // #7A8494
constexpr uint16_t kAccent = 0x3DD5;   // #3FB8AF
constexpr uint16_t kWarn = 0xED07;     // #E8A33D
constexpr uint16_t kDanger = 0xE289;   // #E5534B

constexpr int kWidth = 240;
constexpr int kHeight = 135;
constexpr int kBarHeight = 15;
constexpr int kLine = 14;           // efontCN_12 line
// SSH page font: 10 / 12 / 14 px (Fn+= and Fn+- on the SSH page).  ASCII
// cells are half the size wide; the remote tmux window follows the grid.
struct TermFont {
    const lgfx::IFont* font;
    int line;
    int cell;
};
TermFont term_font();
void apply_terminal_size();
constexpr int kPad = 4;

// `text` cut with "…" to fit `max_px` in the current font.
String fit(Gfx& g, const String& text, int max_px);
// Lines of `text` no wider than `width`; breaks at '\n' and between UTF-8 characters.
std::vector<String> wrap_text(Gfx& g, const String& text, int width);
// Full-height QR code on a white square at the left; returns the x where the
// text column to its right starts.
int qr_panel(Gfx& g, const String& data);
bool has_char(const Keyboard_Class::KeysState& state, char c);
void erase_last(String& text);  // one UTF-8 character

}  // namespace vat::ui
