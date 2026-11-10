#include "cjk_font.h"

#include <algorithm>

namespace vat::cjk {

const Font cn10(cjk_data::kRest10, cjk_data::kCell10);
const Font cn12(cjk_data::kRest12, cjk_data::kCell12);
const Font cn14(cjk_data::kRest14, cjk_data::kCell14);

const uint8_t* Font::glyph(uint16_t code) const {
    if (code < 0x4E00 || code > 0x9FFF) return nullptr;
    const uint16_t* end = cjk_data::kHanzi + sizeof(cjk_data::kHanzi) / sizeof(cjk_data::kHanzi[0]);
    const uint16_t* at = std::lower_bound(cjk_data::kHanzi, end, code);
    if (at == end || *at != code) return nullptr;
    return cell_.bits + (at - cjk_data::kHanzi) * ((cell_.w * cell_.h + 7) / 8);
}

bool Font::updateFontMetric(lgfx::FontMetrics* metrics, uint16_t code) const {
    if (!glyph(code)) return rest_.updateFontMetric(metrics, code);
    metrics->width = cell_.w;
    metrics->x_offset = cell_.x;
    metrics->x_advance = cell_.advance;
    return true;
}

// U8g2font::drawChar with the pixels read from the cell instead of decoded
// from runs: the same offsets, scaling and background fill.
size_t Font::drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t code, const lgfx::TextStyle* style,
                      lgfx::FontMetrics* metrics, int32_t& filled_x) const {
    const uint8_t* bits = glyph(code);
    if (!bits) return rest_.drawChar(gfx, x, y, code, style, metrics, filled_x);

    const int32_t sy = 65536 * style->size_y;
    y += (metrics->y_offset * sy) >> 16;
    const uint32_t w = cell_.w;
    const uint32_t h = cell_.h;
    const int32_t sx = 65536 * style->size_x;
    const int32_t xoffset = (cell_.x * sx) >> 16;
    const int32_t yoffset = -(int32_t)(cell_.y + h + metrics->y_offset);
    const int32_t xAdvance = (cell_.advance * sx) >> 16;

    const uint32_t colortbl[2] = {gfx->getColorConverter()->convert(style->back_rgb888),
                                  gfx->getColorConverter()->convert(style->fore_rgb888)};
    const bool fillbg = (style->back_rgb888 != style->fore_rgb888);
    int32_t left = 0;
    int32_t right = 0;
    if (fillbg) {
        left = std::max<int>(filled_x, x + (xoffset < 0 ? xoffset : 0));
        right = x + std::max<int>(((w * sx) >> 16) + xoffset, xAdvance);
        filled_x = right;
        gfx->setRawColor(colortbl[0]);
    }
    x += xoffset;
    gfx->startWrite();

    if (left < right) {
        if (yoffset > 0) gfx->writeFillRect(left, y, right - left, (yoffset * sy) >> 16);
        const int32_t y0 = ((yoffset + h) * sy) >> 16;
        const int32_t y1 = (metrics->height * sy) >> 16;
        if (y0 < y1) gfx->writeFillRect(left, y + y0, right - left, y1 - y0);
        const int32_t r0 = (yoffset * sy) >> 16;
        const int32_t len = (((yoffset + h) * sy) >> 16) - r0;
        if (left < x) gfx->writeFillRect(left, y + r0, x - left, len);
        const int32_t xwsx = x + ((w * sx) >> 16);
        if (xwsx < right) gfx->writeFillRect(xwsx, y + r0, right - xwsx, len);
    }
    left -= x;

    uint32_t bit = 0;
    for (uint32_t ly = 0; ly < h; ++ly) {
        const int32_t y0 = ((ly + yoffset) * sy) >> 16;
        const int32_t y1 = ((ly + yoffset + 1) * sy) >> 16;
        uint32_t lx = 0;
        while (lx < w) {  // one run of equal pixels at a time, as the runs of u8g2
            const bool on = (bits[bit >> 3] >> (bit & 7)) & 1;
            uint32_t len = 0;
            while (lx + len < w && (((bits[(bit + len) >> 3] >> ((bit + len) & 7)) & 1) != 0) == on) ++len;
            if (on || fillbg) {
                int32_t x0 = (lx * sx) >> 16;
                if (!on && x0 < left) x0 = left;
                const int32_t x1 = ((lx + len) * sx) >> 16;
                if (x0 < x1) {
                    gfx->setRawColor(colortbl[on]);
                    gfx->writeFillRect(x + x0, y + y0, x1 - x0, y1 - y0);
                }
            }
            lx += len;
            bit += len;
        }
    }
    gfx->endWrite();
    return xAdvance;
}

}  // namespace vat::cjk
