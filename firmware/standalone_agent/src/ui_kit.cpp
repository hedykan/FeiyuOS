#include "ui_kit.h"
#include "cjk_font.h"

#include <algorithm>

#include "arena.h"
#include "config.h"
#include "ops_tui.h"

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
    lgfx::LGFX_Sprite* s = strip();
    if (!s || height > kStripHeight) {
        screen.fillRect(0, y, kWidth, height, bg);
        paint(screen, y);
        return;
    }
    s->setFont(&cjk::cn12);
    s->setTextSize(1);
    s->fillRect(0, 0, kWidth, height, bg);
    paint(*s, 0);
    screen.setClipRect(0, y, kWidth, height);  // only `height` rows of the strip
    s->pushSprite(&screen, 0, y);
    // The strip is DMA memory: inside startWrite() the push runs on while
    // this returns, and the next band would repaint the strip under it (each
    // row showed the next one's text, 2026-11-09).  Wait before reusing it.
    screen.waitDMA();
    screen.clearClipRect();
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

int qr_panel(Gfx& g, const String& data) {
    const int side = kHeight - 4;
    g.fillRect(0, 0, side + 4, kHeight, TFT_WHITE);
    g.qrcode(data.c_str(), 2, 2, side, 1);
    return side + 8;
}

std::vector<String> wrap_text(Gfx& g, const String& text, int width) {
    std::vector<String> lines;
    int start = 0;
    while (start <= static_cast<int>(text.length())) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = text.length();
        String rest = text.substring(start, end);
        do {
            int cut_at = rest.length();
            while (cut_at > 0 && g.textWidth(rest.substring(0, cut_at)) > width) {
                --cut_at;
                while (cut_at > 0 && (static_cast<uint8_t>(rest[cut_at]) & 0xC0) == 0x80) --cut_at;
            }
            if (cut_at == 0) cut_at = rest.length();  // a single glyph wider than the line
            lines.push_back(rest.substring(0, cut_at));
            rest = rest.substring(cut_at);
        } while (!rest.isEmpty());
        start = end + 1;
    }
    return lines;
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
