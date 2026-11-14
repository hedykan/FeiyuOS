#include "ui_kit.h"
#include "cjk_font.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "arena.h"
#include "config.h"
#include "ops_tui.h"
#include "ui_compose.h"
#include "lgfx/utility/lgfx_qrcode.h"

namespace vat::ui {

lgfx::LGFX_Sprite* strip() {
    static lgfx::LGFX_Sprite* sprite = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto* s = new lgfx::LGFX_Sprite(&M5Cardputer.Display);
        s->setColorDepth(16);
        if (s->createSprite(kWidth, kStripHeight)) {
            sprite = s;
            arena::note("ui_strip", kWidth * kStripHeight * 2);
        } else {
            delete s;
        }
    }
    return sprite;
}

void band(Gfx& screen, int y, int height, uint16_t bg, const std::function<void(Gfx& g, int top)>& paint) {
    if (!compose::composing(screen)) {  // on its own: an update of just these rows
        compose::area({0, y, kWidth, height}, [&](Gfx& g) { band(g, y, height, bg, paint); }, bg);
        return;
    }
    if (!compose::visible(screen, y, height)) return;
    const lgfx::IFont* font = screen.getFont();
    compose::clip(screen, 0, y, kWidth, height);
    screen.fillRect(0, y, kWidth, height, bg);
    screen.setFont(&cjk::cn12);
    screen.setTextSize(1);
    paint(screen, y);
    compose::unclip(screen);
    screen.setFont(font);
}

namespace {
uint32_t slide_ms() {
    const uint8_t level = config::get().animation;  // 0 off, 1 reduced, 2 on
    return level == 0 ? 0 : level == 1 ? 60 : 120;
}
}  // namespace

void scrolled_rows(Gfx& screen, int top, int slots, int scroll_px, int count, int bar,
                   const std::function<void(Gfx& g, int index, int y)>& paint) {
    for (int k = 0; k < slots; ++k) {
        band(screen, top + k * kLine - 1, kLine, kBg, [&](Gfx& g, int t) {
            const int content = k * kLine + scroll_px;  // the content's y at this slot's top
            if (bar >= 0 && bar - content > -kLine && bar - content < kLine) {
                g.fillRect(0, t + bar - content, kWidth, kLine, kSurface);
                g.fillRect(0, t + bar - content, 2, kLine, kAccent);
            }
            const int first = content >= 0 ? content / kLine : -((kLine - 1 - content) / kLine);
            for (int i = first; i <= first + 1; ++i) {
                if (i >= 0 && i < count) paint(g, i, t + i * kLine - content + 1);
            }
        });
    }
}

int CursorSlide::follow(int target, int context) {
    const uint32_t now = millis();
    const uint32_t ms = slide_ms();
    const auto position = [&] {
        const float t = ms ? std::min(1.0f, (now - started_) / static_cast<float>(ms)) : 1.0f;
        const float eased = 1 - (1 - t) * (1 - t);
        return from_ + static_cast<int>((target_ - from_) * eased);
    };
    if (target != target_) {
        const bool slide = target_ >= 0 && context == context_;
        from_ = slide ? position() : target;  // a key while sliding starts from where the bar is
        target_ = target;
        started_ = now;
    }
    context_ = context;
    sliding_ = now - started_ < ms && from_ != target_;
    return position();
}

TermFont term_font() {
    switch (config::get().terminal_font) {
        case 10: return {&cjk::cn10, 11, 5};
        case 14: return {&cjk::cn14, 15, 7};
        default: return {&cjk::cn12, 13, 6};
    }
}
void apply_terminal_size() {
    const TermFont f = term_font();
    tui::set_size((kWidth - 4) / f.cell, (kHeight - kBarHeight - 2) / f.line);
}

String fit(Gfx& g, const String& text, int max_px) {
    if (g.textWidth(text) <= max_px) return text;
    const int ellipsis = g.textWidth("…");
    String out;
    for (std::size_t i = 0; i < text.length();) {
        std::size_t len = 1;
        const unsigned char c = text[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        const String next = out + text.substring(i, i + len);
        if (g.textWidth(next) + ellipsis > max_px) break;
        out = next;
        i += len;
    }
    return out + "…";
}

String fit_tail(Gfx& g, const String& text, int max_px) {
    if (g.textWidth(text) <= max_px) return text;
    const int ellipsis = g.textWidth("…");
    int start = static_cast<int>(text.length());
    while (start > 0) {
        int prev = start - 1;
        while (prev > 0 && (static_cast<unsigned char>(text[prev]) & 0xC0) == 0x80) --prev;  // whole UTF-8 characters
        if (g.textWidth(text.substring(prev)) + ellipsis > max_px) break;
        start = prev;
    }
    return "…" + text.substring(start);
}

int measure_text(const String& text, int size) {
    static std::mutex lock;
    static lgfx::LGFX_Sprite* ruler = nullptr;  // no pixels: fonts only
    std::lock_guard<std::mutex> guard(lock);
    if (!ruler) ruler = new lgfx::LGFX_Sprite();
    ruler->setFont(&cjk::cn12);
    ruler->setTextSize(std::clamp(size, 1, 4));
    return ruler->textWidth(text);
}

namespace {
struct QrCode {
    String text;
    QRCode code{};
    std::vector<uint8_t> modules;  // code.modules points here (a move keeps the buffer)
    bool ok = false;
};

QrCode encode_qr(const String& data) {
    QrCode qr;
    qr.text = data;
    for (uint8_t version = 1; version <= 40 && !qr.ok; ++version) {
        qr.modules.assign(lgfx_qrcode_getBufferSize(version), 0);
        qr.ok = lgfx_qrcode_initText(&qr.code, qr.modules.data(), version, 0, data.c_str()) == 0;
    }
    return qr;
}
}  // namespace

void draw_qr(Gfx& g, const String& data, int x, int y, int w) {
    g.fillRect(x, y, w, w, TFT_WHITE);
    static compose::PerUpdate<QrCode> encoded;  // once for all the bands
    QrCode& qr = encoded.get(g, [&] { return encode_qr(data); });
    if (qr.text != data) qr = encode_qr(data);  // a second code in the same frame
    if (!qr.ok) return;
    const int size = qr.code.size;
    const int cell = w / size;
    if (cell <= 0) return;
    const int offset = (w - size * cell) / 2;
    for (int iy = 0; iy < size; ++iy) {
        const int dy = y + offset + iy * cell;
        if (!compose::visible(g, dy, cell)) continue;
        for (int ix = 0; ix < size; ++ix) {
            if (lgfx_qrcode_getModule(&qr.code, ix, iy)) g.fillRect(x + offset + ix * cell, dy, cell, cell, TFT_BLACK);
        }
    }
}

int qr_panel(Gfx& g, const String& data) {
    const int side = kHeight - 4;
    g.fillRect(0, 0, side + 4, kHeight, TFT_WHITE);
    draw_qr(g, data, 2, 2, side);
    return side + 8;
}

std::vector<String> wrap(Gfx& g, const String& text, int width, int max_lines) {
    std::vector<String> lines;
    const int n = text.length();
    const char* s = text.c_str();
    const auto full = [&] { return max_lines > 0 && static_cast<int>(lines.size()) > max_lines; };
    int start = 0;
    while (start < n && !full()) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = n;
        if (start == end) lines.push_back(String());
        int line_start = start;
        int px = 0;
        for (int i = start; i < end && !full();) {
            const uint8_t c = s[i];
            const int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            char glyph[5] = {};
            memcpy(glyph, s + i, std::min(len, end - i));
            const int w = g.textWidth(glyph);
            if (px + w > width && i > line_start) {
                lines.push_back(text.substring(line_start, i));
                line_start = i;
                px = 0;
            }
            px += w;
            i += len;
        }
        if (line_start < end && !full()) lines.push_back(text.substring(line_start, end));
        start = end + 1;
    }
    if (full() || (max_lines > 0 && static_cast<int>(lines.size()) == max_lines && start < n)) {
        lines.resize(max_lines);
        lines.back() = fit(g, lines.back() + "…", width);
    }
    return lines;
}

void footer(Gfx& g, const Footer& f, int span) {
    band(g, kHeight - kLine, kLine, kBg, [&](Gfx& b, int top) {
        int right = span - kPad;
        if (!f.hint.isEmpty()) {
            b.setTextColor(kMuted, kBg);
            b.drawRightString(f.hint, right, top);
            right -= b.textWidth(f.hint) + 8;
        }
        b.setTextColor(f.color, kBg);
        switch (f.align) {
            case Align::Left: b.drawString(fit_tail(b, f.text, right - kPad), kPad, top); break;
            case Align::Center: b.drawCentreString(fit(b, f.text, span - 8), span / 2, top); break;
            case Align::Right: b.drawRightString(fit(b, f.text, right - kPad), right, top); break;
        }
    });
}

bool has_char(const Keyboard_Class::KeysState& state, char c) {
    return std::find(state.word.begin(), state.word.end(), c) != state.word.end();
}

void erase_last(String& text) {
    int end = text.length();
    while (end > 0) {
        --end;
        if ((static_cast<unsigned char>(text[end]) & 0xC0) != 0x80) break;
    }
    text.remove(end);
}

}  // namespace vat::ui
