#pragma once

#include <cstdint>

// Generated data for cjk_font (tools/gen_cjk_font.py).
namespace vat::cjk_data {

// One size's hanzi: every glyph a w x h bitmap (row-major, LSB first, each
// glyph starting on a byte), drawn x px right of the pen and with its bottom
// row y px above the baseline (u8g2's convention); all advance by `advance`.
struct Cell {
    uint8_t w;
    uint8_t h;
    int8_t x;
    int8_t y;
    int8_t advance;
    const uint8_t* bits;
};

extern const uint16_t kHanzi[6763];  // sorted; glyph i of every size is kHanzi[i]
extern const uint8_t kRest10[], kRest12[], kRest14[];  // u8g2 fonts without the hanzi
extern const Cell kCell10, kCell12, kCell14;

}  // namespace vat::cjk_data
