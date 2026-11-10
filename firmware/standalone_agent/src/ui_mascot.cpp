#include "ui_mascot.h"

#include <algorithm>
#include <cstring>

#include "sprite_sheet.h"
#include "storage.h"

namespace vat::ui::mascot {
namespace {

constexpr char kPath[] = "/vat/assets/mascot.bin";
constexpr int kStates = 5;
constexpr int kMaxColors = 64;
constexpr std::size_t kFrameBytes = kSize * kSize;
constexpr uint16_t kMinFrameMs = 100;          // whatever the file says
constexpr unsigned long kErrorShownMs = 10000;  // then back to idle
constexpr unsigned long kRetryMs = 30000;       // look for the file again

// The file (tools/pack_mascot.py) is parsed by SpriteSheet; it must hold
// the five states in State's order.

bool ready = false;
unsigned long last_try = 0;
bool tried = false;
SpriteSheet sheet;
lgfx::rgb565_t palette[kMaxColors];
uint8_t pixels[kFrameBytes];  // static: nothing on the heap while a request is in flight
bool have_frame = false;
uint16_t backdrop[kSize];  // page colour behind each mascot row

State requested = State::Idle;
State shown = State::Idle;
int frame = 0;
unsigned long frame_since = 0;
unsigned long error_since = 0;

void show(State state) {
    shown = state;
    frame = 0;
    frame_since = millis();
}

void open_file() {
    ready = false;
    if (!sheet.open(kPath) || sheet.states() != kStates) return;
    for (int c = 0; c < kMaxColors; ++c) palette[c] = lgfx::rgb565_t(sheet.color(c));
    reset_backdrop(kBg);  // transparent pixels show the page behind (set per row by the page)
    ready = true;
    show(requested);
}

}  // namespace

bool enabled() {
    if (!ready && (!tried || millis() - last_try > kRetryMs)) {
        tried = true;
        last_try = millis();
        if (storage::ready()) open_file();
    }
    return ready;
}

void request(State state) {
    if (state == requested) return;
    requested = state;
    if (state == State::Error) error_since = millis();
    show(state);
}

bool due() {
    if (!ready) return false;
    const unsigned long now = millis();
    if (shown == State::Error && now - error_since > kErrorShownMs) {
        show(State::Idle);
        return true;
    }
    const int s = static_cast<int>(shown);
    if (now - frame_since < std::max(sheet.duration(s, frame), kMinFrameMs)) return false;
    frame = (frame + 1) % sheet.frames(s);
    frame_since = now;
    return true;
}

bool load() {
    if (!ready) return false;
    // card busy: keep showing the last frame
    have_frame = sheet.read_frame(static_cast<int>(shown), frame, pixels) || have_frame;
    return have_frame;
}

void push(Gfx& g) {
    for (int y = 0; y < kSize; ++y) {  // one row at a time: index 0 becomes what is behind that row
        if (!have_frame) {
            g.fillRect(kLeft, kTop + y, kSize, 1, backdrop[y]);
            continue;
        }
        palette[0] = lgfx::rgb565_t(backdrop[y]);
        g.pushImage(kLeft, kTop + y, kSize, 1, pixels + y * kSize, lgfx::color_depth_t::palette_8bit, palette);
    }
}

void reset_backdrop(uint16_t color) {
    for (auto& c : backdrop) c = color;
}

void set_backdrop(int screen_y, int height, uint16_t color) {
    for (int y = std::max(screen_y, kTop); y < std::min(screen_y + height, kTop + kSize); ++y) backdrop[y - kTop] = color;
}

const char* describe() {
    if (!ready) return "off";
    static const char* const names[] = {"idle", "recognize", "understand", "execute", "error"};
    return names[static_cast<int>(shown)];
}

}  // namespace vat::ui::mascot
