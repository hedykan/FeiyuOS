#include "ui_mascot.h"

#include <algorithm>
#include <cstring>

#include "storage.h"

namespace vat::ui::mascot {
namespace {

constexpr char kPath[] = "/vat/assets/mascot.bin";
constexpr int kStates = 5;
constexpr int kMaxFrames = 8;
constexpr int kMaxColors = 64;
constexpr std::size_t kFrameBytes = kSize * kSize;
constexpr uint16_t kMinFrameMs = 100;          // whatever the file says
constexpr unsigned long kErrorShownMs = 10000;  // then back to idle
constexpr unsigned long kRetryMs = 30000;       // look for the file again

// File layout (little-endian), written by tools/pack_mascot.py:
//     0  "VATM"
//     4  version 1, frame size 32, state count 5, colour count (<= 64; index 0 = transparent)
//     8  frames per state [8]
//    16  frame durations in ms, uint16 [8 states][8 frames]
//   144  palette, uint16 RGB565 [64]
//   272  frames, state by state, one palette index per pixel, rows top-down
constexpr std::size_t kHeaderBytes = 272;

bool ready = false;
unsigned long last_try = 0;
bool tried = false;
uint8_t frame_counts[kStates] = {};
uint16_t durations[kStates][kMaxFrames] = {};
lgfx::rgb565_t palette[kMaxColors];
uint8_t pixels[kFrameBytes];  // static: nothing on the heap while a request is in flight
bool have_frame = false;
uint16_t backdrop[kSize];  // page colour behind each mascot row

State requested = State::Idle;
State shown = State::Idle;
int frame = 0;
unsigned long frame_since = 0;
unsigned long error_since = 0;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void show(State state) {
    shown = state;
    frame = 0;
    frame_since = millis();
}

void open_file() {
    ready = false;
    fs::File file = storage::fs().open(kPath, FILE_READ);
    if (!file) return;
    uint8_t h[kHeaderBytes];
    const bool full = file.read(h, sizeof h) == sizeof h;
    const std::size_t size = file.size();
    file.close();
    if (!full || std::memcmp(h, "VATM", 4) != 0 || h[4] != 1 || h[5] != kSize || h[6] != kStates || h[7] == 0 ||
        h[7] > kMaxColors) {
        return;
    }
    std::size_t total = 0;
    for (int s = 0; s < kStates; ++s) {
        frame_counts[s] = h[8 + s];
        if (frame_counts[s] == 0 || frame_counts[s] > kMaxFrames) return;
        total += frame_counts[s];
        for (int f = 0; f < kMaxFrames; ++f) {
            durations[s][f] = std::max(le16(h + 16 + (s * kMaxFrames + f) * 2), kMinFrameMs);
        }
    }
    if (size < kHeaderBytes + total * kFrameBytes) return;
    for (int c = 0; c < kMaxColors; ++c) palette[c] = lgfx::rgb565_t(le16(h + 144 + c * 2));
    reset_backdrop(kBg);  // transparent pixels show the page behind (set per row by the page)
    ready = true;
    show(requested);
}

std::size_t frame_offset() {
    std::size_t index = frame;
    for (int s = 0; s < static_cast<int>(shown); ++s) index += frame_counts[s];
    return kHeaderBytes + index * kFrameBytes;
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
    if (now - frame_since < durations[s][frame]) return false;
    frame = (frame + 1) % frame_counts[s];
    frame_since = now;
    return true;
}

bool load() {
    if (!ready) return false;
    fs::File file = storage::fs().open(kPath, FILE_READ);
    if (!file) return have_frame;  // card busy: keep showing the last frame
    const bool ok = file.seek(frame_offset()) && file.read(pixels, kFrameBytes) == kFrameBytes;
    file.close();
    have_frame = have_frame || ok;
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
