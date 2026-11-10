#include "sprite_sheet.h"

#include <algorithm>
#include <cstring>

#include "storage.h"

namespace vat::ui {
namespace {

// Layout (little-endian), written by tools/pack_mascot.py:
//     0  "VATM", version 1, frame size 32, state count, colour count
//     8  frames per state [8]
//    16  frame durations in ms, uint16 [8 states][8 frames]
//   144  palette, uint16 RGB565 [64]
//   272  frames, state by state, one palette index per pixel, rows top-down
// Version 2 puts int8 dx, dy per frame [8 states][8 frames] at 272 and the
// frames at 400.
constexpr std::size_t kHeaderBytes = 272;
constexpr std::size_t kHeaderV2Bytes = 400;
constexpr std::size_t kFrameBytes = SpriteSheet::kSize * SpriteSheet::kSize;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

bool SpriteSheet::open(const char* path) {
    states_ = 0;
    fs::File file = storage::fs().open(path, FILE_READ);
    if (!file) return false;
    uint8_t h[kHeaderV2Bytes];
    const std::size_t got = file.read(h, sizeof h);
    const std::size_t size = file.size();
    file.close();
    const std::size_t header = got >= 5 && h[4] == 2 ? kHeaderV2Bytes : kHeaderBytes;
    if (got < header || std::memcmp(h, "VATM", 4) != 0 || (h[4] != 1 && h[4] != 2) || h[5] != kSize || h[6] == 0 || h[6] > kMaxStates ||
        h[7] == 0 || h[7] > 64) {
        return false;
    }
    std::size_t total = 0;
    for (int s = 0; s < h[6]; ++s) {
        counts_[s] = h[8 + s];
        if (counts_[s] == 0 || counts_[s] > kMaxFrames) return false;
        total += counts_[s];
        for (int f = 0; f < kMaxFrames; ++f) {
            durations_[s][f] = le16(h + 16 + (s * kMaxFrames + f) * 2);
            for (int axis = 0; axis < 2; ++axis) {
                offsets_[s][f][axis] = header == kHeaderV2Bytes
                    ? static_cast<int8_t>(h[kHeaderBytes + (s * kMaxFrames + f) * 2 + axis]) : 0;
            }
        }
    }
    if (size < header + total * kFrameBytes) return false;
    for (int c = 0; c < 64; ++c) palette_[c] = le16(h + 144 + c * 2);
    path_ = path;
    header_ = header;
    states_ = h[6];
    return true;
}

uint16_t SpriteSheet::duration(int state, int frame) const {
    if (state < 0 || state >= states_ || frame < 0 || frame >= counts_[state]) return 100;
    return std::max<uint16_t>(durations_[state][frame], 60);
}

void SpriteSheet::offset(int state, int frame, int& dx, int& dy) const {
    dx = dy = 0;
    if (state < 0 || state >= states_ || frame < 0 || frame >= counts_[state]) return;
    dx = offsets_[state][frame][0];
    dy = offsets_[state][frame][1];
}

bool SpriteSheet::read_state(int state, uint8_t* out, std::size_t capacity) const {
    if (state < 0 || state >= states_) return false;
    const std::size_t bytes = counts_[state] * kFrameBytes;
    if (bytes > capacity) return false;
    std::size_t index = 0;
    for (int s = 0; s < state; ++s) index += counts_[s];
    fs::File file = storage::fs().open(path_, FILE_READ);
    if (!file) return false;
    const bool ok = file.seek(header_ + index * kFrameBytes) && file.read(out, bytes) == bytes;
    file.close();
    return ok;
}

bool SpriteSheet::read_frame(int state, int frame, uint8_t* out) const {
    if (state < 0 || state >= states_ || frame < 0 || frame >= counts_[state]) return false;
    std::size_t index = frame;
    for (int s = 0; s < state; ++s) index += counts_[s];
    fs::File file = storage::fs().open(path_, FILE_READ);
    if (!file) return false;
    const bool ok = file.seek(header_ + index * kFrameBytes) && file.read(out, kFrameBytes) == kFrameBytes;
    file.close();
    return ok;
}

void SpriteSheet::draw(lgfx::LGFXBase& g, const uint8_t* frame, int x, int y, int scale, uint16_t bg) const {
    for (int r = 0; r < kSize; ++r) {
        const uint8_t* row = frame + r * kSize;
        for (int c = 0; c < kSize;) {  // runs of one colour as one rectangle
            const uint8_t index = row[c];
            int run = 1;
            while (c + run < kSize && row[c + run] == index) ++run;
            g.fillRect(x + c * scale, y + r * scale, run * scale, scale, index ? palette_[index] : bg);
            c += run;
        }
    }
}

}  // namespace vat::ui
