#pragma once

// Theme, sizes and small text/keyboard helpers shared by the UI pages
// (ui.cpp and settings_page.cpp).
#include <M5Cardputer.h>

#include <functional>
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
constexpr int kLine = 14;           // cjk::cn12 line
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
// The end of `text` that fits, "…" in front (for text that grows at the end).
String fit_tail(Gfx& g, const String& text, int max_px);
// Lines of `text` no wider than `width`; breaks at '\n' and between UTF-8 characters.
std::vector<String> wrap_text(Gfx& g, const String& text, int width);
// Full-height QR code on a white square at the left; returns the x where the
// text column to its right starts.
int qr_panel(Gfx& g, const String& data);
// Off-screen strip, kWidth × kStripHeight at 16 bits (7.5 KB, claimed on first
// use and kept, so it never moves): a band is drawn there, then pushed whole,
// and the screen never shows it half drawn (no flicker).  nullptr when the
// memory is not there.
constexpr int kStripHeight = 16;
lgfx::LGFX_Sprite* strip();
// Paints the band [y, y + height) (height <= kStripHeight) on `bg` through the
// strip; `paint` gets the target and the y of the band's top on it (0 in the
// strip, `y` on the screen when there is no strip).  cjk::cn12, size 1.
void band(Gfx& screen, int y, int height, uint16_t bg, const std::function<void(Gfx& g, int top)>& paint);

// A list's cursor bar slides to its new place (easing out over 120 ms; 60 ms
// with the animation setting at 减弱, none at 关).  `context` stands for the
// list's scroll position and page: when it changes the bar jumps instead.
// The settings menus, the Agent timeline and panels each keep one.
class CursorSlide {
public:
    int follow(int target, int context);  // where to draw the bar now
    bool sliding() const { return sliding_; }  // keep redrawing until false
    void reset() { target_ = -1; }             // the next follow() jumps

private:
    int target_ = -1;
    int from_ = -1;
    int context_ = -1;
    uint32_t started_ = 0;
    bool sliding_ = false;
};
// A list in `slots` fixed rows from text row y `top`, scrolled `scroll_px`
// pixels into its content: a scroll slides instead of jumping (a CursorSlide
// on first_row × kLine).  The cursor bar is at content y `bar` (-1: none).
// paint(g, index, y) paints row `index` with its text at y, transparent over
// the bar; each row goes through the strip, so nothing flickers.
void scrolled_rows(Gfx& screen, int top, int slots, int scroll_px, int count, int bar,
                   const std::function<void(Gfx& g, int index, int y)>& paint);
bool has_char(const Keyboard_Class::KeysState& state, char c);
void erase_last(String& text);  // one UTF-8 character

}  // namespace vat::ui
