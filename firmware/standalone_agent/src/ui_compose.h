#pragma once

// The only way anything is drawn on the screen after the boot splash
// (docs/V075_PLAN.zh-CN.md G1).  A painter draws in screen coordinates on the
// Gfx it is given; the compositor runs it once per 16-row band into the strip
// (ui_kit strip(), 7.5 KB) and pushes each band whole, so the screen never
// shows a band cleared or half drawn, and bands that did not change are not
// pushed at all.  The plan and its rules: ui_compose_plan.h.
#include <functional>

#include "ui_compose_plan.h"
#include "ui_kit.h"

namespace vat::ui::compose {

using Painter = std::function<void(Gfx& g)>;

// The whole screen.  Unchanged bands are skipped.
void frame(const Painter& paint, uint16_t bg = kBg);
// Part of the screen (a status bar, the mascot's corner, a blinking dot):
// only `area` is written.
void area(const Rect& area, const Painter& paint, uint16_t bg = kBg);
// The screen no longer shows what the ledger says (it was off, or the boot
// splash drew on it): the next frame pushes every band.
void invalidate();

// Inside frame() / area(): `g` is a band.  Painters clip only through these,
// which keep the clip inside the band (setClipRect on a band would let a
// painter write outside the strip's memory).
void clip(Gfx& g, int x, int y, int w, int h);
void unclip(Gfx& g);
bool composing(const Gfx& g);
// False when rows [y, y + h) are outside the band being composed: a painter
// skips them (layout and all), which is what keeps nine passes cheap.  Always
// true when not composing.
bool visible(const Gfx& g, int y, int h);
// Composing on `g`: runs `paint` on the strip as a real 16-row sprite, clipped
// to the band, with g's colours; `top` is the band's top (paint draws at
// y - top).  For calls that bound their writes by the sprite's size instead of
// its clip (M5GFX's anti-aliased lines wrote outside the strip and broke the
// heap).  False when not composing on a strip: draw on g as usual.
bool on_real_band(const Gfx& g, const std::function<void(Gfx& real, int top)>& paint);

// Work a painter does once for all the bands of one update (a layout, wrapped
// lines): made on the first band, kept for that update's other bands, freed
// when the update ends, so nothing laid out stays on the heap between frames
// (S8 heap_low_water fell from 43.8 to 35.9 KB while layouts were kept).
class Scratch {
public:
    virtual ~Scratch() = default;
    virtual void release() = 0;

protected:
    void enlist();
};

template <typename T>
class PerUpdate : public Scratch {
public:
    template <typename Make>
    T& get(const Gfx& g, Make make) {
        if (!listed_) {
            enlist();
            listed_ = true;
        }
        if (!made_ || !composing(g)) {
            value_ = make();
            made_ = composing(g);
        }
        return value_;
    }
    void release() override {
        value_ = T();
        made_ = false;
    }

private:
    T value_{};
    bool made_ = false;
    bool listed_ = false;
};

Stats stats();
String stats_json();  // for device://local/status

}  // namespace vat::ui::compose
