#pragma once

// The screen compositor's plan: which bands an update touches, which of them
// changed, and what was pushed.  No M5GFX here, so the rules that keep the
// screen from flickering are tested on the host (tests/host/ui_compose_test.cpp):
//
//   1. nothing is cleared on the screen: a band is composed off screen and
//      pushed whole, every pixel once, with its final colour;
//   2. a band whose content did not change is not pushed;
//   3. an update writes only inside its area.
//
// ui_compose.cpp runs this plan against the display (docs/V075_PLAN.zh-CN.md G1).
#include <cstdint>
#include <functional>
#include <vector>

namespace vat::ui::compose {

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 135;
constexpr int kBandHeight = 16;  // the strip (ui_kit kStripHeight)
constexpr int kBands = (kScreenHeight + kBandHeight - 1) / kBandHeight;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    Rect intersect(const Rect& o) const;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
constexpr Rect kScreen{0, 0, kScreenWidth, kScreenHeight};

// The part of `area` in each band it touches, top to bottom.
struct Slice {
    int band;
    Rect rect;
};
std::vector<Slice> slices(const Rect& area);

uint32_t hash(const uint16_t* pixels, int count);  // FNV-1a over the band's pixels

// What the screen shows, band by band, as the hash of the last full-width push.
class Ledger {
public:
    // A full band was composed with `hash`: true when it differs from what is
    // on the screen (push it), and it is what the screen shows from now on.
    bool changed(int band, uint32_t hash);
    void forget(int band);  // part of the band was pushed: its content is no longer known
    void forget_all();      // the screen was off, or drawn by something else

private:
    uint32_t shown_[kBands] = {};
    bool known_[kBands] = {};
};

struct Stats {
    uint32_t frames = 0;   // whole-screen updates
    uint32_t areas = 0;    // partial updates
    uint32_t pushed = 0;   // slices sent to the display
    uint32_t frame_pushed = 0;  // ...of them by whole-screen updates
    uint32_t skipped = 0;  // bands composed but unchanged
    uint32_t last_ms = 0;
    uint32_t max_ms = 0;
};

// The display side of one update (ui_compose.cpp; a recording fake in the tests).
class Target {
public:
    virtual ~Target() = default;
    // Composing `band`: writes are clipped to `clip` (inside the band), which
    // starts filled with `bg`.
    virtual void begin(int band, const Rect& clip, uint16_t bg) = 0;
    virtual uint32_t content_hash(int band) = 0;            // the whole band as composed
    virtual void push(int band, const Rect& rect) = 0;      // that part to the screen
};

// One update of `area`: each band it touches is composed (paint draws in
// screen coordinates) and pushed, except a full-width band whose content is
// what the screen already shows.  An update of the whole screen counts as a frame.
void run(Target& target, Ledger& ledger, Stats& stats, const Rect& area, uint16_t bg,
         const std::function<void()>& paint);

}  // namespace vat::ui::compose
