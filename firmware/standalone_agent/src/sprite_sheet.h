#pragma once

#include <M5GFX.h>

#include <cstdint>
#include <vector>

// A packed animation file (tools/pack_mascot.py, "VATM" version 1 or 2): up
// to 8 states of up to 8 frames, 32×32 pixels, one palette of up to 64
// colours (index 0 transparent); version 2 adds each frame's offset.  The
// boot screen reads the states it needs into RAM once so its drawing task
// never touches the card; the Agent page's mascot reads one frame at a time.
namespace vat::ui {

class SpriteSheet {
public:
    static constexpr int kSize = 32;
    static constexpr int kMaxStates = 8;
    static constexpr int kMaxFrames = 8;

    // Reads the header of `path`; false when missing or not a valid file.
    bool open(const char* path);
    int states() const { return states_; }
    int frames(int state) const { return state >= 0 && state < states_ ? counts_[state] : 0; }
    uint16_t duration(int state, int frame) const;
    // Where `frame` sits relative to where the state ends, in art pixels
    // (version 2; 0, 0 in version 1).
    void offset(int state, int frame, int& dx, int& dy) const;
    // All frames of `state`, kSize×kSize palette indices each, into `out`
    // (at most `capacity` bytes); false when they do not fit or cannot be read.
    bool read_state(int state, uint8_t* out, std::size_t capacity) const;
    // One frame of `state` (kSize×kSize indices) into `out`: the mascot reads
    // a frame at a time to keep its RAM small.
    bool read_frame(int state, int frame, uint8_t* out) const;
    uint16_t color(int index) const { return index >= 0 && index < 64 ? palette_[index] : 0; }
    // One frame (from read_state's buffer) `scale` times larger at (x, y);
    // transparent pixels are painted `bg` (so a redraw needs no clearing).
    void draw(lgfx::LGFXBase& g, const uint8_t* frame, int x, int y, int scale, uint16_t bg) const;

private:
    String path_;
    int states_ = 0;
    uint8_t counts_[kMaxStates] = {};
    uint16_t durations_[kMaxStates][kMaxFrames] = {};
    int8_t offsets_[kMaxStates][kMaxFrames][2] = {};
    std::size_t header_ = 0;
    uint16_t palette_[64] = {};
};

}  // namespace vat::ui
