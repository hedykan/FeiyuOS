#include "ui_compose.h"

#include <M5Cardputer.h>

#include "cjk_font.h"

namespace vat::ui::compose {
namespace {

Ledger ledger;
Stats totals;
lgfx::LGFX_Sprite* view = nullptr;  // the strip's memory, addressed as a whole screen
uint16_t* pixels = nullptr;         // the strip's 16 rows
Rect band_clip;
uint32_t lowest_heap = UINT32_MAX;  // free heap after painting a band, lowest since boot
int depth = 0;
Gfx* drawing = nullptr;  // what painters draw on during an update: the band, or the screen without a strip
std::vector<Scratch*> scratches;  // freed after every update

void release_scratch() {
    for (auto* s : scratches) s->release();
}

bool ready() {
    if (view) return true;
    lgfx::LGFX_Sprite* s = strip();
    if (!s || !s->getBuffer()) return false;
    pixels = static_cast<uint16_t*>(s->getBuffer());
    view = new lgfx::LGFX_Sprite(&M5Cardputer.Display);
    return true;
}

class DisplayTarget : public Target {
public:
    void begin(int band, const Rect& clip, uint16_t bg) override {
        // Row r of the screen lands in strip row r - band × 16.  Rows outside
        // the band have no memory behind them: the clip keeps every write
        // inside (and painters clip only through compose::clip()).
        view->setBuffer(pixels - band * kBandHeight * kScreenWidth, kScreenWidth, kScreenHeight, 16);
        band_clip = clip;
        view->setClipRect(clip.x, clip.y, clip.w, clip.h);
        view->fillRect(clip.x, clip.y, clip.w, clip.h, bg);
        // Every band starts from the same state, so the bands of one frame agree.
        view->setFont(&cjk::cn12);
        view->setTextSize(1);
        view->setTextDatum(lgfx::top_left);
        view->setTextWrap(false, false);
        view->setTextColor(kText, bg);
    }
    uint32_t content_hash(int band) override {
        lowest_heap = std::min<uint32_t>(lowest_heap, ESP.getFreeHeap());  // a band is painted: its layouts are alive
        const int rows = std::min(kBandHeight, kScreenHeight - band * kBandHeight);
        return hash(pixels, rows * kScreenWidth);
    }
    void push(int band, const Rect& rect) override {
        auto& display = M5Cardputer.Display;
        display.setClipRect(rect.x, rect.y, rect.w, rect.h);
        strip()->pushSprite(&display, 0, band * kBandHeight);
        // The push runs on by DMA: the next band must not repaint the strip under it.
        display.waitDMA();
        display.clearClipRect();
    }
};

void update(const Rect& rect, const Painter& paint, uint16_t bg) {
    auto& display = M5Cardputer.Display;
    if (depth > 0) {  // called from inside a painter: it is part of what is being composed
        paint(*drawing);
        return;
    }
    const uint32_t started = millis();
    ++depth;
    if (!ready()) {  // no strip memory: draw on the screen (it may flicker, but it works)
        drawing = &display;
        band_clip = rect.intersect(kScreen);
        display.startWrite();
        display.setClipRect(band_clip.x, band_clip.y, band_clip.w, band_clip.h);
        display.fillRect(band_clip.x, band_clip.y, band_clip.w, band_clip.h, bg);
        paint(display);
        display.clearClipRect();
        display.endWrite();
        drawing = nullptr;
        --depth;
        ledger.forget_all();
        release_scratch();
        return;
    }
    drawing = view;
    display.startWrite();
    DisplayTarget target;
    run(target, ledger, totals, rect, bg, [&] { paint(*view); });
    display.endWrite();
    drawing = nullptr;
    --depth;
    release_scratch();
    totals.last_ms = millis() - started;
    totals.max_ms = std::max(totals.max_ms, totals.last_ms);
}

}  // namespace

void frame(const Painter& paint, uint16_t bg) { update(kScreen, paint, bg); }

void area(const Rect& rect, const Painter& paint, uint16_t bg) { update(rect, paint, bg); }

void invalidate() { ledger.forget_all(); }

bool composing(const Gfx& g) { return depth > 0 && drawing && &g == drawing; }

void clip(Gfx& g, int x, int y, int w, int h) {
    if (!composing(g)) {
        g.setClipRect(x, y, w, h);
        return;
    }
    const Rect r = Rect{x, y, w, h}.intersect(band_clip);
    g.setClipRect(r.x, r.y, r.w, r.h);
}

void unclip(Gfx& g) {
    if (composing(g)) g.setClipRect(band_clip.x, band_clip.y, band_clip.w, band_clip.h);
    else g.clearClipRect();
}

bool visible(const Gfx& g, int y, int h) {
    if (!composing(g)) return true;
    return y < band_clip.y + band_clip.h && y + h > band_clip.y;
}

void Scratch::enlist() { scratches.push_back(this); }

bool on_real_band(const Gfx& g, const std::function<void(Gfx& real, int top)>& paint) {
    if (!composing(g) || drawing != view) return false;  // not composing, or drawing on the screen itself
    lgfx::LGFX_Sprite* real = strip();
    const int top = band_clip.y / kBandHeight * kBandHeight;
    real->setClipRect(band_clip.x, band_clip.y - top, band_clip.w, band_clip.h);
    real->setRawColor(view->getRawColor());
    real->setBaseColor(view->getBaseColor());
    paint(*real, top);
    real->clearClipRect();
    return true;
}

Stats stats() { return totals; }

String stats_json() {
    return "{\"frames\":" + String(totals.frames) + ",\"areas\":" + String(totals.areas) +
           ",\"pushed\":" + String(totals.pushed) + ",\"frame_pushed\":" + String(totals.frame_pushed) + ",\"skipped\":" + String(totals.skipped) +
           ",\"last_ms\":" + String(totals.last_ms) + ",\"max_ms\":" + String(totals.max_ms) +
           ",\"min_heap\":" + String(lowest_heap == UINT32_MAX ? 0 : lowest_heap) + "}";
}

}  // namespace vat::ui::compose
