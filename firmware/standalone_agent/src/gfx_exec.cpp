#include "gfx_exec.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cjk_font.h"
#include "gfx_api.h"
#include "storage.h"
#include "ui_compose.h"

namespace vat::gfx_exec {
namespace {

using ui::draw_qr;
using ui::kHeight;
using ui::kWidth;
using ui_canvas::Kind;
using ui_canvas::Op;

// ---------------------------------------------------------------- arguments

struct Args {
    const std::vector<String>& v;
    const Blobs& blobs;
    std::size_t size() const { return v.size(); }
    bool has(std::size_t i) const { return i < v.size(); }
    int32_t i(std::size_t k, int32_t fallback = 0) const {
        int out = fallback;
        return has(k) && ui_canvas::number(v[k], out) ? out : fallback;
    }
    float f(std::size_t k, float fallback = 0) const {
        if (!has(k)) return fallback;
        char* end = nullptr;
        const float out = std::strtof(v[k].c_str(), &end);
        return end == v[k].c_str() ? fallback : out;
    }
    uint16_t c(std::size_t k) const {  // #RRGGBB (after placeholders are filled) as RGB565
        uint32_t rgb = 0xFFFFFF;
        if (has(k)) ui_canvas::color(v[k], rgb);
        return gfx_api::rgb565(rgb);
    }
    bool b(std::size_t k) const { return has(k) && v[k] != "0" && v[k] != "false"; }
    const lgfx::IFont* font(std::size_t k) const {
        const String name = has(k) ? v[k] : String("cn12");
        if (name == "cn14") return &cjk::cn14;
        if (name == "Font0") return &fonts::Font0;
        if (name == "Font2") return &fonts::Font2;
        if (name == "Font4") return &fonts::Font4;
        if (name == "Font6") return &fonts::Font6;
        if (name == "Font7") return &fonts::Font7;
        if (name == "Font8") return &fonts::Font8;
        return &cjk::cn12;
    }
    const std::vector<uint8_t>* blob(std::size_t k) const {
        if (!has(k) || !v[k].startsWith("@")) return nullptr;
        const int n = v[k].substring(1).toInt();
        return n >= 0 && n < static_cast<int>(blobs.size()) ? &blobs[n] : nullptr;
    }
    const char* s(std::size_t k) const { return has(k) ? v[k].c_str() : ""; }
};

// ---------------------------------------------------------------- rotation

struct Turn {
    bool on = false;
    int angle = 0, cx = 0, cy = 0;
    void at(int32_t& x, int32_t& y) const {
        if (!on) return;
        int xi = x, yi = y;
        ui_canvas::rotate_point(angle, cx, cy, xi, yi);
        x = xi;
        y = yi;
    }
};

// A shape turned into a polygon; `color` nullptr: setColor's.
void polygon(Gfx& g, const int32_t* xs, const int32_t* ys, int n, const uint16_t* color, bool fill) {
    if (fill) {
        for (int i = 1; i + 1 < n; ++i) {
            if (color) g.fillTriangle(xs[0], ys[0], xs[i], ys[i], xs[i + 1], ys[i + 1], *color);
            else g.fillTriangle(xs[0], ys[0], xs[i], ys[i], xs[i + 1], ys[i + 1]);
        }
    } else {
        for (int i = 0; i < n; ++i) {
            if (color) g.drawLine(xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], *color);
            else g.drawLine(xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n]);
        }
    }
}

void turned_rect(Gfx& g, const Turn& t, int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* c, bool fill) {
    int32_t xs[4] = {x, x + w - 1, x + w - 1, x}, ys[4] = {y, y, y + h - 1, y + h - 1};
    for (int i = 0; i < 4; ++i) t.at(xs[i], ys[i]);
    polygon(g, xs, ys, 4, c, fill);
}

void turned_ellipse(Gfx& g, const Turn& t, int32_t cx, int32_t cy, int32_t rx, int32_t ry, const uint16_t* c, bool fill) {
    constexpr int kSteps = 36;
    int32_t xs[kSteps], ys[kSteps];
    for (int i = 0; i < kSteps; ++i) {
        const float a = 6.2831853f * i / kSteps;
        xs[i] = cx + static_cast<int32_t>(std::lround(rx * std::cos(a)));
        ys[i] = cy + static_cast<int32_t>(std::lround(ry * std::sin(a)));
        t.at(xs[i], ys[i]);
    }
    polygon(g, xs, ys, kSteps, c, fill);
}

// ---------------------------------------------------------------- the M5GFX calls

using Runner = void (*)(Gfx& g, const Args& a, int overload, Turn& t);

#define COLOR_AT(k) (a.has(k) ? &(color_store = a.c(k)) : nullptr)

void fill_screen(Gfx& g, const Args& a, int, Turn&) {
    if (a.has(0)) g.fillScreen(a.c(0));
    else g.fillScreen();
}
void clear(Gfx& g, const Args& a, int, Turn&) {
    if (a.has(0)) g.clear(a.c(0));
    else g.clear();
}
void draw_pixel(Gfx& g, const Args& a, int, Turn& t) {
    int32_t x = a.i(0), y = a.i(1);
    t.at(x, y);
    if (a.has(2)) g.drawPixel(x, y, a.c(2));
    else g.drawPixel(x, y);
}
void fast_line(Gfx& g, const Args& a, Turn& t, bool vertical) {
    int32_t x = a.i(0), y = a.i(1);
    const int32_t n = a.i(2);
    if (t.on) {
        int32_t x1 = vertical ? x : x + n - 1, y1 = vertical ? y + n - 1 : y;
        t.at(x, y);
        t.at(x1, y1);
        if (a.has(3)) g.drawLine(x, y, x1, y1, a.c(3));
        else g.drawLine(x, y, x1, y1);
        return;
    }
    if (vertical) {
        if (a.has(3)) g.drawFastVLine(x, y, n, a.c(3));
        else g.drawFastVLine(x, y, n);
    } else {
        if (a.has(3)) g.drawFastHLine(x, y, n, a.c(3));
        else g.drawFastHLine(x, y, n);
    }
}
void draw_fast_vline(Gfx& g, const Args& a, int, Turn& t) { fast_line(g, a, t, true); }
void draw_fast_hline(Gfx& g, const Args& a, int, Turn& t) { fast_line(g, a, t, false); }

void rect(Gfx& g, const Args& a, Turn& t, bool fill, bool round) {
    const std::size_t at = round ? 5 : 4;
    uint16_t color_store = 0;
    const uint16_t* c = COLOR_AT(at);
    if (t.on) return turned_rect(g, t, a.i(0), a.i(1), a.i(2), a.i(3), c, fill);
    const int32_t x = a.i(0), y = a.i(1), w = a.i(2), h = a.i(3), r = a.i(4);
    if (round && fill) c ? g.fillRoundRect(x, y, w, h, r, *c) : g.fillRoundRect(x, y, w, h, r);
    else if (round) c ? g.drawRoundRect(x, y, w, h, r, *c) : g.drawRoundRect(x, y, w, h, r);
    else if (fill) c ? g.fillRect(x, y, w, h, *c) : g.fillRect(x, y, w, h);
    else c ? g.drawRect(x, y, w, h, *c) : g.drawRect(x, y, w, h);
}
void draw_rect(Gfx& g, const Args& a, int, Turn& t) { rect(g, a, t, false, false); }
void fill_rect(Gfx& g, const Args& a, int, Turn& t) { rect(g, a, t, true, false); }
void draw_round_rect(Gfx& g, const Args& a, int, Turn& t) { rect(g, a, t, false, true); }
void fill_round_rect(Gfx& g, const Args& a, int, Turn& t) { rect(g, a, t, true, true); }

void circle(Gfx& g, const Args& a, Turn& t, bool fill) {
    int32_t x = a.i(0), y = a.i(1);
    t.at(x, y);
    if (fill) a.has(3) ? g.fillCircle(x, y, a.i(2), a.c(3)) : g.fillCircle(x, y, a.i(2));
    else a.has(3) ? g.drawCircle(x, y, a.i(2), a.c(3)) : g.drawCircle(x, y, a.i(2));
}
void draw_circle(Gfx& g, const Args& a, int, Turn& t) { circle(g, a, t, false); }
void fill_circle(Gfx& g, const Args& a, int, Turn& t) { circle(g, a, t, true); }

void ellipse(Gfx& g, const Args& a, Turn& t, bool fill) {
    uint16_t color_store = 0;
    const uint16_t* c = COLOR_AT(4);
    if (t.on) return turned_ellipse(g, t, a.i(0), a.i(1), a.i(2), a.i(3), c, fill);
    if (fill) c ? g.fillEllipse(a.i(0), a.i(1), a.i(2), a.i(3), *c) : g.fillEllipse(a.i(0), a.i(1), a.i(2), a.i(3));
    else c ? g.drawEllipse(a.i(0), a.i(1), a.i(2), a.i(3), *c) : g.drawEllipse(a.i(0), a.i(1), a.i(2), a.i(3));
}
void draw_ellipse(Gfx& g, const Args& a, int, Turn& t) { ellipse(g, a, t, false); }
void fill_ellipse(Gfx& g, const Args& a, int, Turn& t) { ellipse(g, a, t, true); }

void draw_line(Gfx& g, const Args& a, int, Turn& t) {
    int32_t x0 = a.i(0), y0 = a.i(1), x1 = a.i(2), y1 = a.i(3);
    t.at(x0, y0);
    t.at(x1, y1);
    a.has(4) ? g.drawLine(x0, y0, x1, y1, a.c(4)) : g.drawLine(x0, y0, x1, y1);
}

void triangle(Gfx& g, const Args& a, Turn& t, bool fill) {
    int32_t p[6];
    for (int k = 0; k < 6; ++k) p[k] = a.i(k);
    for (int k = 0; k < 6; k += 2) t.at(p[k], p[k + 1]);
    if (fill) a.has(6) ? g.fillTriangle(p[0], p[1], p[2], p[3], p[4], p[5], a.c(6)) : g.fillTriangle(p[0], p[1], p[2], p[3], p[4], p[5]);
    else a.has(6) ? g.drawTriangle(p[0], p[1], p[2], p[3], p[4], p[5], a.c(6)) : g.drawTriangle(p[0], p[1], p[2], p[3], p[4], p[5]);
}
void draw_triangle(Gfx& g, const Args& a, int, Turn& t) { triangle(g, a, t, false); }
void fill_triangle(Gfx& g, const Args& a, int, Turn& t) { triangle(g, a, t, true); }

void draw_bezier(Gfx& g, const Args& a, int overload, Turn& t) {
    const int points = overload == 1 ? 4 : 3;
    int32_t p[8];
    for (int k = 0; k < points * 2; ++k) p[k] = a.i(k);
    for (int k = 0; k < points * 2; k += 2) t.at(p[k], p[k + 1]);
    const std::size_t at = points * 2;
    if (points == 3) a.has(at) ? g.drawBezier(p[0], p[1], p[2], p[3], p[4], p[5], a.c(at)) : g.drawBezier(p[0], p[1], p[2], p[3], p[4], p[5]);
    else g.drawBezier(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], a.c(at));  // M5GFX: the colour is required here
}

void ellipse_arc(Gfx& g, const Args& a, bool fill) {
    const int32_t x = a.i(0), y = a.i(1), r0x = a.i(2), r1x = a.i(3), r0y = a.i(4), r1y = a.i(5);
    const float a0 = a.f(6), a1 = a.f(7);
    if (fill) a.has(8) ? g.fillEllipseArc(x, y, r0x, r1x, r0y, r1y, a0, a1, a.c(8)) : g.fillEllipseArc(x, y, r0x, r1x, r0y, r1y, a0, a1);
    else a.has(8) ? g.drawEllipseArc(x, y, r0x, r1x, r0y, r1y, a0, a1, a.c(8)) : g.drawEllipseArc(x, y, r0x, r1x, r0y, r1y, a0, a1);
}
void draw_ellipse_arc(Gfx& g, const Args& a, int, Turn&) { ellipse_arc(g, a, false); }
void fill_ellipse_arc(Gfx& g, const Args& a, int, Turn&) { ellipse_arc(g, a, true); }

void arc(Gfx& g, const Args& a, Turn& t, bool fill) {
    int32_t x = a.i(0), y = a.i(1);
    t.at(x, y);
    const float extra = t.on ? t.angle : 0;  // an arc turns by moving its angles
    const float a0 = a.f(4) + extra, a1 = a.f(5) + extra;
    if (fill) a.has(6) ? g.fillArc(x, y, a.i(2), a.i(3), a0, a1, a.c(6)) : g.fillArc(x, y, a.i(2), a.i(3), a0, a1);
    else a.has(6) ? g.drawArc(x, y, a.i(2), a.i(3), a0, a1, a.c(6)) : g.drawArc(x, y, a.i(2), a.i(3), a0, a1);
}
void draw_arc(Gfx& g, const Args& a, int, Turn& t) { arc(g, a, t, false); }
void fill_arc(Gfx& g, const Args& a, int, Turn& t) { arc(g, a, t, true); }

void gradient_hline(Gfx& g, const Args& a, int, Turn&) { g.drawGradientHLine(a.i(0), a.i(1), a.i(2), a.c(3), a.c(4)); }
void gradient_vline(Gfx& g, const Args& a, int, Turn&) { g.drawGradientVLine(a.i(0), a.i(1), a.i(2), a.c(3), a.c(4)); }
void gradient_line(Gfx& g, const Args& a, int, Turn& t) {
    int32_t x0 = a.i(0), y0 = a.i(1), x1 = a.i(2), y1 = a.i(3);
    t.at(x0, y0);
    t.at(x1, y1);
    g.drawGradientLine(x0, y0, x1, y1, a.c(4), a.c(5));
}
void gradient_rect(Gfx& g, const Args& a, int, Turn&) {
    const auto style = static_cast<lgfx::v1::gradient_fill_styles::fill_style_t>(a.has(6) ? a.i(6) : 2);
    g.fillGradientRect(a.i(0), a.i(1), a.i(2), a.i(3), a.c(4), a.c(5), style);
}
void smooth_line(Gfx& g, const Args& a, int, Turn&) { g.drawSmoothLine(a.i(0), a.i(1), a.i(2), a.i(3), a.c(4)); }
void wide_line(Gfx& g, const Args& a, int, Turn&) { g.drawWideLine(a.i(0), a.i(1), a.i(2), a.i(3), a.f(4), a.c(5)); }
void wedge_line(Gfx& g, const Args& a, int, Turn&) { g.drawWedgeLine(a.i(0), a.i(1), a.i(2), a.i(3), a.f(4), a.f(5), a.c(6)); }
void spot(Gfx& g, const Args& a, int, Turn&) { g.drawSpot(a.i(0), a.i(1), a.f(2), a.c(3)); }
void smooth_circle(Gfx& g, const Args& a, int, Turn&) {
    a.has(3) ? g.fillSmoothCircle(a.i(0), a.i(1), a.i(2), a.c(3)) : g.fillSmoothCircle(a.i(0), a.i(1), a.i(2));
}
void smooth_round_rect(Gfx& g, const Args& a, int, Turn&) {
    a.has(5) ? g.fillSmoothRoundRect(a.i(0), a.i(1), a.i(2), a.i(3), a.i(4), a.c(5))
             : g.fillSmoothRoundRect(a.i(0), a.i(1), a.i(2), a.i(3), a.i(4));
}
void rect_alpha(Gfx& g, const Args& a, int, Turn&) {
    g.fillRectAlpha(a.i(0), a.i(1), a.i(2), a.i(3), static_cast<uint8_t>(std::clamp<int32_t>(a.i(4), 0, 255)), a.c(5));
}
void circle_helper(Gfx& g, const Args& a, int, Turn&) {
    const uint_fast8_t corner = static_cast<uint_fast8_t>(a.i(3));
    a.has(4) ? g.drawCircleHelper(a.i(0), a.i(1), a.i(2), corner, a.c(4)) : g.drawCircleHelper(a.i(0), a.i(1), a.i(2), corner);
}
void fill_circle_helper(Gfx& g, const Args& a, int, Turn&) {
    const uint_fast8_t corners = static_cast<uint_fast8_t>(a.i(3));
    a.has(5) ? g.fillCircleHelper(a.i(0), a.i(1), a.i(2), corners, a.i(4), a.c(5))
             : g.fillCircleHelper(a.i(0), a.i(1), a.i(2), corners, a.i(4));
}
void fill_affine(Gfx& g, const Args& a, int, Turn&) {
    float m[6] = {1, 0, 0, 0, 1, 0};
    const String& text = a.v[0];
    int from = 0;
    for (int k = 0; k < 6; ++k) {
        const int comma = text.indexOf(',', from);
        m[k] = std::strtof(text.substring(from, comma < 0 ? text.length() : comma).c_str(), nullptr);
        if (comma < 0) break;
        from = comma + 1;
    }
    a.has(3) ? g.fillAffine(m, a.i(1), a.i(2), a.c(3)) : g.fillAffine(m, a.i(1), a.i(2));
}

void bitmap(Gfx& g, const Args& a, bool x_bitmap) {
    const auto* bytes = a.blob(2);
    const int32_t w = a.i(3), h = a.i(4);
    if (!bytes || w <= 0 || h <= 0 || static_cast<int32_t>(bytes->size()) < (x_bitmap ? (w + 7) / 8 * h : (w + 7) / 8 * h)) return;
    if (x_bitmap) a.has(6) ? g.drawXBitmap(a.i(0), a.i(1), bytes->data(), w, h, a.c(5), a.c(6)) : g.drawXBitmap(a.i(0), a.i(1), bytes->data(), w, h, a.c(5));
    else a.has(6) ? g.drawBitmap(a.i(0), a.i(1), bytes->data(), w, h, a.c(5), a.c(6)) : g.drawBitmap(a.i(0), a.i(1), bytes->data(), w, h, a.c(5));
}
void draw_bitmap(Gfx& g, const Args& a, int, Turn&) { bitmap(g, a, false); }
void draw_xbitmap(Gfx& g, const Args& a, int, Turn&) { bitmap(g, a, true); }

lgfx::rgb565_t transparent(const Args& a, std::size_t k) {
    uint32_t rgb = 0;
    ui_canvas::color(a.v[k], rgb);
    return lgfx::rgb565_t((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
void push_image(Gfx& g, const Args& a, int, Turn&) {
    const auto* bytes = a.blob(4);
    const int32_t w = a.i(2), h = a.i(3);
    if (!bytes || w <= 0 || h <= 0 || static_cast<int32_t>(bytes->size()) < w * h * 2) return;
    const auto* pixels = reinterpret_cast<const lgfx::rgb565_t*>(bytes->data());
    a.has(5) ? g.pushImage(a.i(0), a.i(1), w, h, pixels, transparent(a, 5)) : g.pushImage(a.i(0), a.i(1), w, h, pixels);
}
void push_image_rotate_zoom(Gfx& g, const Args& a, int, Turn&) {
    const auto* bytes = a.blob(9);
    const int32_t w = a.i(7), h = a.i(8);
    if (!bytes || w <= 0 || h <= 0 || static_cast<int32_t>(bytes->size()) < w * h * 2) return;
    const auto* pixels = reinterpret_cast<const lgfx::rgb565_t*>(bytes->data());
    if (a.has(10)) {
        g.pushImageRotateZoom(a.f(0), a.f(1), a.f(2), a.f(3), a.f(4), a.f(5), a.f(6), w, h, pixels, transparent(a, 10));
    } else {
        g.pushImageRotateZoom(a.f(0), a.f(1), a.f(2), a.f(3), a.f(4), a.f(5), a.f(6), w, h, pixels);
    }
}
void image(Gfx& g, const Args& a, char kind) {
    const auto* bytes = a.blob(0);
    if (!bytes || bytes->empty()) return;
    const auto datum = static_cast<lgfx::datum_t>(a.has(9) ? a.i(9) : 0);
    const int32_t x = a.i(1), y = a.i(2), mw = a.i(3), mh = a.i(4), ox = a.i(5), oy = a.i(6);
    const float sx = a.f(7, 1.0f), sy = a.f(8, 0.0f);
    if (kind == 'p') g.drawPng(bytes->data(), bytes->size(), x, y, mw, mh, ox, oy, sx, sy, datum);
    else if (kind == 'j') g.drawJpg(bytes->data(), bytes->size(), x, y, mw, mh, ox, oy, sx, sy, datum);
    else g.drawBmp(bytes->data(), bytes->size(), x, y, mw, mh, ox, oy, sx, sy, datum);
}
void draw_png(Gfx& g, const Args& a, int, Turn&) { image(g, a, 'p'); }
void draw_jpg(Gfx& g, const Args& a, int, Turn&) { image(g, a, 'j'); }
void draw_bmp(Gfx& g, const Args& a, int, Turn&) { image(g, a, 'b'); }
void qrcode(Gfx& g, const Args& a, int, Turn& t) {
    const int32_t side = a.has(3) ? a.i(3) : kHeight * 9 / 10;
    int32_t x = a.has(1) ? a.i(1) : (kWidth - side) / 2, y = a.has(2) ? a.i(2) : (kHeight - side) / 2;
    t.at(x, y);
    draw_qr(g, a.s(0), x, y, std::clamp<int32_t>(side, 21, kHeight));
}

void text(Gfx& g, const Args& a, Turn& t, char align) {
    int32_t x = a.i(1), y = a.i(2);
    t.at(x, y);
    const lgfx::IFont* f = a.has(3) ? a.font(3) : nullptr;
    switch (align) {
        case 'l': f ? g.drawString(a.s(0), x, y, f) : g.drawString(a.s(0), x, y); break;
        case 'c': f ? g.drawCentreString(a.s(0), x, y, f) : g.drawCentreString(a.s(0), x, y); break;
        default: f ? g.drawRightString(a.s(0), x, y, f) : g.drawRightString(a.s(0), x, y); break;
    }
}
void draw_string(Gfx& g, const Args& a, int, Turn& t) { text(g, a, t, 'l'); }
void draw_centre_string(Gfx& g, const Args& a, int, Turn& t) { text(g, a, t, 'c'); }
void draw_right_string(Gfx& g, const Args& a, int, Turn& t) { text(g, a, t, 'r'); }
void draw_number(Gfx& g, const Args& a, int, Turn&) {
    a.has(3) ? g.drawNumber(a.i(0), a.i(1), a.i(2), a.font(3)) : g.drawNumber(a.i(0), a.i(1), a.i(2));
}
void draw_float(Gfx& g, const Args& a, int, Turn&) {
    const uint8_t dp = static_cast<uint8_t>(std::clamp<int32_t>(a.i(1), 0, 7));
    a.has(4) ? g.drawFloat(a.f(0), dp, a.i(2), a.i(3), a.font(4)) : g.drawFloat(a.f(0), dp, a.i(2), a.i(3));
}
void draw_char(Gfx& g, const Args& a, int overload, Turn&) {
    if (overload == 0) {
        const uint16_t code = static_cast<uint16_t>(a.i(0));
        if (!a.has(3)) {
            g.drawChar(code, a.i(1), a.i(2));
            return;
        }
        const auto* saved = g.getFont();  // M5GFX takes a font here only by number: switch for this one
        g.setFont(a.font(3));
        g.drawChar(code, a.i(1), a.i(2));
        g.setFont(saved);
        return;
    }
    const float sx = a.f(5, 1), sy = a.has(6) ? a.f(6, 1) : sx;
    g.drawChar(a.i(0), a.i(1), static_cast<uint16_t>(a.i(2)), a.c(3), a.c(4), sx, sy);
}
void print(Gfx& g, const Args& a, int, Turn&) { g.print(a.s(0)); }
void println(Gfx& g, const Args& a, int, Turn&) {
    if (a.has(0)) g.println(a.s(0));
    else g.println();
}

void set_cursor(Gfx& g, const Args& a, int, Turn&) {
    a.has(2) ? g.setCursor(a.i(0), a.i(1), a.font(2)) : g.setCursor(a.i(0), a.i(1));
}
void set_text_color(Gfx& g, const Args& a, int, Turn&) {
    a.has(1) ? g.setTextColor(a.c(0), a.c(1)) : g.setTextColor(a.c(0));
}
void set_text_size(Gfx& g, const Args& a, int, Turn&) {
    const float sx = std::clamp(a.f(0, 1), 0.25f, 8.0f);
    a.has(1) ? g.setTextSize(sx, std::clamp(a.f(1, 1), 0.25f, 8.0f)) : g.setTextSize(sx);
}
void set_text_datum(Gfx& g, const Args& a, int, Turn&) { g.setTextDatum(static_cast<uint8_t>(a.i(0))); }
void set_text_wrap(Gfx& g, const Args& a, int, Turn&) { g.setTextWrap(a.b(0), a.b(1)); }
void set_text_padding(Gfx& g, const Args& a, int, Turn&) { g.setTextPadding(static_cast<uint32_t>(std::max<int32_t>(0, a.i(0)))); }
void set_text_scroll(Gfx& g, const Args& a, int, Turn&) { g.setTextScroll(a.b(0)); }
void set_font(Gfx& g, const Args& a, int, Turn&) { g.setFont(a.font(0)); }
void set_color(Gfx& g, const Args& a, int overload, Turn&) {
    if (overload == 1) g.setColor(static_cast<uint8_t>(a.i(0)), static_cast<uint8_t>(a.i(1)), static_cast<uint8_t>(a.i(2)));
    else g.setColor(a.c(0));
}
void set_base_color(Gfx& g, const Args& a, int, Turn&) { g.setBaseColor(a.c(0)); }
void set_clip_rect(Gfx& g, const Args& a, int, Turn&) { ui::compose::clip(g, a.i(0), a.i(1), a.i(2), a.i(3)); }
void clear_clip_rect(Gfx& g, const Args&, int, Turn&) { ui::compose::unclip(g); }

struct Named {
    const char* name;
    Runner run;
};
const Named kRunners[] = {
    {"fillScreen", fill_screen},
    {"clear", clear},
    {"drawPixel", draw_pixel},
    {"drawFastVLine", draw_fast_vline},
    {"drawFastHLine", draw_fast_hline},
    {"drawRect", draw_rect},
    {"fillRect", fill_rect},
    {"drawRoundRect", draw_round_rect},
    {"fillRoundRect", fill_round_rect},
    {"drawCircle", draw_circle},
    {"fillCircle", fill_circle},
    {"drawEllipse", draw_ellipse},
    {"fillEllipse", fill_ellipse},
    {"drawLine", draw_line},
    {"drawTriangle", draw_triangle},
    {"fillTriangle", fill_triangle},
    {"drawBezier", draw_bezier},
    {"drawEllipseArc", draw_ellipse_arc},
    {"fillEllipseArc", fill_ellipse_arc},
    {"drawArc", draw_arc},
    {"fillArc", fill_arc},
    {"drawGradientHLine", gradient_hline},
    {"drawGradientVLine", gradient_vline},
    {"drawGradientLine", gradient_line},
    {"fillGradientRect", gradient_rect},
    {"drawSmoothLine", smooth_line},
    {"drawWideLine", wide_line},
    {"drawWedgeLine", wedge_line},
    {"drawSpot", spot},
    {"fillSmoothCircle", smooth_circle},
    {"fillSmoothRoundRect", smooth_round_rect},
    {"fillRectAlpha", rect_alpha},
    {"drawCircleHelper", circle_helper},
    {"fillCircleHelper", fill_circle_helper},
    {"fillAffine", fill_affine},
    {"drawBitmap", draw_bitmap},
    {"drawXBitmap", draw_xbitmap},
    {"pushImage", push_image},
    {"pushImageRotateZoom", push_image_rotate_zoom},
    {"drawPng", draw_png},
    {"drawJpg", draw_jpg},
    {"drawBmp", draw_bmp},
    {"qrcode", qrcode},
    {"drawString", draw_string},
    {"drawCentreString", draw_centre_string},
    {"drawCenterString", draw_centre_string},
    {"drawRightString", draw_right_string},
    {"drawNumber", draw_number},
    {"drawFloat", draw_float},
    {"drawChar", draw_char},
    {"print", print},
    {"println", println},
    {"setCursor", set_cursor},
    {"setTextColor", set_text_color},
    {"setTextSize", set_text_size},
    {"setTextDatum", set_text_datum},
    {"setTextWrap", set_text_wrap},
    {"setTextPadding", set_text_padding},
    {"setTextScroll", set_text_scroll},
    {"setFont", set_font},
    {"setTextFont", set_font},
    {"setColor", set_color},
    {"setBaseColor", set_base_color},
    {"setClipRect", set_clip_rect},
    {"clearClipRect", clear_clip_rect},
};

// Calls that bound their writes by the sprite's size rather than its clip (or
// may: they read pixels back, or write blocks), with the arguments that are y
// coordinates.  While composing they are drawn on the real 16-row strip with
// y - the band's top, so nothing can land outside its memory.
struct Shifted {
    const char* name;
    uint8_t ys[2];  // argument indexes; 255: none
};
const Shifted kShifted[] = {
    {"drawSmoothLine", {1, 3}}, {"drawWideLine", {1, 3}}, {"drawWedgeLine", {1, 3}}, {"drawSpot", {1, 255}},
    {"fillSmoothCircle", {1, 255}}, {"fillSmoothRoundRect", {1, 255}}, {"fillRectAlpha", {1, 255}},
    {"pushImage", {1, 255}}, {"pushImageRotateZoom", {1, 255}}, {"drawPng", {2, 255}}, {"drawJpg", {2, 255}},
    {"drawBmp", {2, 255}}, {"fillAffine", {255, 255}},  // its matrix's 6th number is y
};

const Shifted* shifted(int row) {
    static std::vector<int8_t> by_row;  // -1: not shifted
    if (by_row.empty()) {
        by_row.assign(gfx_api::count(), -1);
        for (std::size_t i = 0; i < gfx_api::count(); ++i) {
            for (std::size_t k = 0; k < sizeof(kShifted) / sizeof(kShifted[0]); ++k) {
                if (!std::strcmp(kShifted[k].name, gfx_api::table()[i].name)) by_row[i] = static_cast<int8_t>(k);
            }
        }
    }
    return row >= 0 && by_row[row] >= 0 ? &kShifted[by_row[row]] : nullptr;
}

// Runner of each gfx_api row (by name, once): a row without one draws nothing
// and is reported once (check at E2E S13 gfx_all_functions).
const std::vector<Runner>& runners() {
    static std::vector<Runner> by_row;
    if (by_row.empty()) {
        by_row.assign(gfx_api::count(), nullptr);
        for (std::size_t i = 0; i < gfx_api::count(); ++i) {
            for (const auto& r : kRunners) {
                if (!std::strcmp(r.name, gfx_api::table()[i].name)) by_row[i] = r.run;
            }
            if (!by_row[i] && gfx_api::table()[i].group != gfx_api::Group::Query) {
                runtime_event("gfx_missing", gfx_api::table()[i].name);
            }
        }
    }
    return by_row;
}

uint8_t hex_value(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

}  // namespace

bool prepare(std::vector<Op>& ops, Blobs& blobs, String& error) {
    std::size_t total = 0;
    for (const auto& b : blobs) total += b.size();
    for (auto& op : ops) {
        if (op.kind != Kind::Call || op.fn < 0) continue;
        const String types = gfx_api::overload_params(gfx_api::table()[op.fn], op.overload);
        for (std::size_t k = 0; k < op.args.size() && k < types.length(); ++k) {
            const char type = types[k];
            if (type != 'B' && type != 'D' && type != 'P') continue;
            if (op.args[k].startsWith("@")) continue;  // already a blob (a program's)
            std::vector<uint8_t> bytes;
            if (type == 'P') {
                const std::size_t room = gfx_api::kMaxBytes > total ? gfx_api::kMaxBytes - total : 0;
                if (!storage::read_bytes(op.args[k], bytes, room)) bytes.clear();
                if (bytes.size() >= room && room) {
                    error = op.args[k] + " 太大：一帧的图片和像素合计不能超过 32 KB";
                    return false;
                }
            } else {  // hex; 'D': each pixel as 4 hex digits, kept little-endian for rgb565_t
                const String& hex = op.args[k];
                bytes.reserve(hex.length() / 2);
                for (std::size_t i = 0; i + 1 < hex.length(); i += 2) {
                    bytes.push_back(static_cast<uint8_t>(hex_value(hex[i]) << 4 | hex_value(hex[i + 1])));
                }
                if (type == 'D') {
                    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) std::swap(bytes[i], bytes[i + 1]);
                }
            }
            total += bytes.size();
            if (total > gfx_api::kMaxBytes) {
                error = "一帧的位图、像素和图片合计不能超过 32 KB";
                return false;
            }
            op.args[k] = "@" + String(static_cast<int>(blobs.size()));
            blobs.push_back(std::move(bytes));
        }
    }
    return true;
}

void reset(Gfx& g) {
    g.setFont(&cjk::cn12);
    g.setTextSize(1);
    g.setTextDatum(lgfx::top_left);
    constexpr uint16_t kWhite = 0xFFFF, kBlack = 0x0000;  // uint16_t: RGB565 (an unsigned int would be RGB888)
    g.setTextColor(kWhite);
    g.setColor(kWhite);
    g.setBaseColor(kBlack);
    g.setTextWrap(false, false);
    g.setTextPadding(0);
    g.setCursor(0, 0);
}

void run(Gfx& g, const std::vector<Op>& ops, const Blobs& blobs, const Picture& picture) {
    const auto& by_row = runners();
    Turn turn;
    for (const auto& op : ops) {
        const Args a{op.args, blobs};
        switch (op.kind) {
            case Kind::Call: {
                if (op.fn < 0 || op.fn >= static_cast<int>(by_row.size()) || !by_row[op.fn]) break;
                const Shifted* shift = shifted(op.fn);
                const auto on_real = [&](Gfx& real, int top) {
                    std::vector<String> moved = op.args;
                    for (uint8_t k : shift->ys) {
                        int y = 0;
                        if (k < moved.size() && ui_canvas::number(moved[k], y)) moved[k] = String(y - top);
                    }
                    if (!std::strcmp(gfx_api::table()[op.fn].name, "fillAffine") && !moved.empty()) {
                        const int last = moved[0].lastIndexOf(',');  // the matrix's ty
                        if (last >= 0) moved[0] = moved[0].substring(0, last + 1) + String(moved[0].substring(last + 1).toFloat() - top, 3);
                    }
                    Turn none;
                    by_row[op.fn](real, Args{moved, blobs}, op.overload, none);
                };
                if (shift && ui::compose::on_real_band(g, on_real)) break;
                by_row[op.fn](g, a, op.overload, turn);
                break;
            }
            case Kind::Rotate:
                turn.angle = a.i(0);
                turn.on = turn.angle % 360 != 0;
                turn.cx = a.i(1);
                turn.cy = a.i(2);
                break;
            case Kind::DrawText: {  // drawString with {size, color, datum}: this text only
                int32_t x = a.i(1), y = a.i(2);
                turn.at(x, y);
                const auto* font = g.getFont();
                const float sx = g.getTextSizeX(), sy = g.getTextSizeY();
                const auto datum = g.getTextDatum();
                const auto style = g.getTextStyle();
                g.setFont(&cjk::cn12);
                g.setTextSize(op.size);
                const int d = gfx_api::datum_of(op.datum);
                g.setTextDatum(static_cast<uint8_t>(d < 0 ? 0 : d));
                uint32_t rgb = 0xFFFFFF;
                ui_canvas::color(op.color, rgb);
                g.setTextColor(gfx_api::rgb565(rgb));
                g.drawString(a.s(0), x, y);
                g.setFont(font);
                g.setTextSize(sx, sy);
                g.setTextDatum(datum);
                g.setTextStyle(style);
                break;
            }
            case Kind::DrawQr: {
                int32_t x = a.i(1), y = a.i(2);
                turn.at(x, y);
                draw_qr(g, a.s(0), x, y, std::clamp<int32_t>(a.i(3, 64), 21, kHeight));
                break;
            }
            case Kind::DrawImage: {
                if (!picture.bytes) break;
                const auto& b = *picture.bytes;
                int32_t x = picture.x, y = picture.y;
                turn.at(x, y);
                if (b.size() > 4 && b[0] == 0x89 && b[1] == 'P') g.drawPng(b.data(), b.size(), x, y);
                else if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8) g.drawJpg(b.data(), b.size(), x, y);
                else if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') g.drawBmp(b.data(), b.size(), x, y);
                break;
            }
            case Kind::ProgressBar: {
                uint32_t rgb = 0xFFFFFF, back = 0x1A1F26;
                ui_canvas::color(op.color, rgb);
                if (!op.color2.isEmpty()) ui_canvas::color(op.color2, back);
                const uint16_t c = gfx_api::rgb565(rgb), bg = gfx_api::rgb565(back);
                const int32_t value = std::clamp<int32_t>(a.i(4), 0, 100);
                if (turn.on) {
                    turned_rect(g, turn, a.i(0), a.i(1), a.i(2), a.i(3), &bg, true);
                    if (value) turned_rect(g, turn, a.i(0), a.i(1), a.i(2) * value / 100, a.i(3), &c, true);
                } else {
                    g.fillRect(a.i(0), a.i(1), a.i(2), a.i(3), bg);
                    g.fillRect(a.i(0), a.i(1), a.i(2) * value / 100, a.i(3), c);
                }
                break;
            }
        }
    }
}

}  // namespace vat::gfx_exec
