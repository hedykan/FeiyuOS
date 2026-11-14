#include "ui_compose_plan.h"

#include <algorithm>

namespace vat::ui::compose {

Rect Rect::intersect(const Rect& o) const {
    const int left = std::max(x, o.x);
    const int top = std::max(y, o.y);
    const int right = std::min(x + w, o.x + o.w);
    const int bottom = std::min(y + h, o.y + o.h);
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

std::vector<Slice> slices(const Rect& area) {
    std::vector<Slice> out;
    const Rect clipped = area.intersect(kScreen);
    if (clipped.empty()) return out;
    for (int band = clipped.y / kBandHeight; band < kBands; ++band) {
        const Rect band_rect{0, band * kBandHeight, kScreenWidth, kBandHeight};
        const Rect part = clipped.intersect(band_rect);
        if (part.empty()) break;
        out.push_back({band, part});
    }
    return out;
}

uint32_t hash(const uint16_t* pixels, int count) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < count; ++i) {
        h = (h ^ (pixels[i] & 0xFF)) * 16777619u;
        h = (h ^ (pixels[i] >> 8)) * 16777619u;
    }
    return h;
}

bool Ledger::changed(int band, uint32_t value) {
    if (band < 0 || band >= kBands) return true;
    const bool differs = !known_[band] || shown_[band] != value;
    shown_[band] = value;
    known_[band] = true;
    return differs;
}

void Ledger::forget(int band) {
    if (band >= 0 && band < kBands) known_[band] = false;
}

void Ledger::forget_all() {
    for (bool& known : known_) known = false;
}

void run(Target& target, Ledger& ledger, Stats& stats, const Rect& area, uint16_t bg,
         const std::function<void()>& paint) {
    const bool whole = area == kScreen;
    if (whole) ++stats.frames;
    else ++stats.areas;
    for (const Slice& slice : slices(area)) {
        target.begin(slice.band, slice.rect, bg);
        paint();
        const int band_rows = std::min(kBandHeight, kScreenHeight - slice.band * kBandHeight);
        const bool full_band = slice.rect.x == 0 && slice.rect.w == kScreenWidth && slice.rect.h == band_rows;
        if (full_band && !ledger.changed(slice.band, target.content_hash(slice.band))) {
            ++stats.skipped;
            continue;
        }
        if (!full_band) ledger.forget(slice.band);
        target.push(slice.band, slice.rect);
        ++stats.pushed;
        if (whole) ++stats.frame_pushed;
    }
}

}  // namespace vat::ui::compose
