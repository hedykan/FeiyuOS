#pragma once

#include <M5GFX.h>

#include "cjk_font_data.h"

// efontCN at 10, 12 and 14 px in ~400 KB of flash instead of 619 KB: the
// hanzi as fixed-cell bitmaps, the rest a u8g2 font (tools/gen_cjk_font.py).
// Drop-in for fonts::efontCN_*: same metrics, same pixels.
namespace vat::cjk {

class Font : public lgfx::IFont {
public:
    constexpr Font(const uint8_t* rest, const cjk_data::Cell& cell) : rest_(rest), cell_(cell) {}
    font_type_t getType() const override { return ft_u8g2; }
    void getDefaultMetric(lgfx::FontMetrics* metrics) const override { rest_.getDefaultMetric(metrics); }
    bool updateFontMetric(lgfx::FontMetrics* metrics, uint16_t code) const override;
    size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t code, const lgfx::TextStyle* style,
                    lgfx::FontMetrics* metrics, int32_t& filled_x) const override;

private:
    const uint8_t* glyph(uint16_t code) const;  // nullptr: not a hanzi of the table
    lgfx::U8g2font rest_;
    const cjk_data::Cell& cell_;
};

extern const Font cn10;
extern const Font cn12;
extern const Font cn14;

}  // namespace vat::cjk
