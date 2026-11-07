#include "ui_kit.h"

#include <algorithm>

#include "config.h"
#include "ops_tui.h"

namespace vat::ui {

TermFont term_font() {
    switch (config::get().terminal_font) {
        case 10: return {&fonts::efontCN_10, 11, 5};
        case 14: return {&fonts::efontCN_14, 15, 7};
        default: return {&fonts::efontCN_12, 13, 6};
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
