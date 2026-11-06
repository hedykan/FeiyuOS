#include "ui_overlay.h"

#include <M5Cardputer.h>

#include <algorithm>

#include "ui_kit.h"

namespace vat::ui::overlay {
namespace {

Overlay current;
bool is_open = false;
bool dirty = false;

const char* overlay_name(Overlay::Kind kind) {
    switch (kind) {
        case Overlay::Kind::Text: return "text";
        case Overlay::Kind::Qr: return "qr";
        case Overlay::Kind::Image: return "image";
        default: return "";
    }
}

void draw_overlay() {
    auto& g = M5Cardputer.Display;
    g.startWrite();
    g.fillScreen(kBg);
    if (current.kind == Overlay::Kind::Text) {
        // Largest size that fits the whole text.  Only fonts already linked
        // for the terminal: another CJK font costs ~450 KB of flash.
        static const lgfx::IFont* const fonts_by_size[] = {&fonts::efontCN_12, &fonts::efontCN_14, &fonts::efontCN_12};
        static const uint8_t scales[] = {2, 1, 1};
        static const int heights[] = {26, 16, 14};
        std::vector<String> lines;
        int font = 0;
        for (; font < 3; ++font) {
            g.setFont(fonts_by_size[font]);
            g.setTextSize(scales[font]);
            lines = wrap_text(g, current.text, kWidth - kPad * 2);
            if (static_cast<int>(lines.size()) * heights[font] <= kHeight - kLine) break;
        }
        if (font == 3) font = 2;
        g.setFont(fonts_by_size[font]);
        g.setTextSize(scales[font]);
        const int shown = std::min<int>(lines.size(), (kHeight - kLine) / heights[font]);
        int y = std::max(2, (kHeight - kLine - shown * heights[font]) / 2);
        g.setTextColor(kText, kBg);
        for (int i = 0; i < shown; ++i, y += heights[font]) {
            if (shown <= 3) g.drawCentreString(lines[i], kWidth / 2, y);
            else g.drawString(lines[i], kPad, y);
        }
        g.setTextSize(1);
        g.setFont(&fonts::efontCN_12);
        g.setTextColor(kMuted, kBg);
        g.drawRightString("任意键返回", kWidth - kPad, kHeight - kLine);
    } else if (current.kind == Overlay::Kind::Qr) {
        const int x = qr_panel(g, current.text);
        g.setFont(&fonts::efontCN_12);
        g.setTextColor(kText, kBg);
        int y = 4;
        for (const auto& line : wrap_text(g, current.text, kWidth - x - 2)) {
            if (y > kHeight - kLine * 2) break;
            g.drawString(line, x, y);
            y += kLine;
        }
        g.setTextColor(kMuted, kBg);
        g.drawString("任意键返回", x, kHeight - kLine);
    } else if (current.kind == Overlay::Kind::Image) {
        const auto& b = current.image;
        const auto datum = textdatum_t::middle_center;
        bool ok = false;
        if (b.size() > 4 && b[0] == 0x89 && b[1] == 'P') ok = g.drawPng(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        else if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8) ok = g.drawJpg(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        else if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') ok = g.drawBmp(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        if (!ok) {
            g.setFont(&fonts::efontCN_12);
            g.setTextColor(kWarn, kBg);
            g.drawCentreString("图片无法解码", kWidth / 2, kHeight / 2 - 6);
        }
        current.image.clear();  // drawn once; the screen keeps it
        current.image.shrink_to_fit();
    }
    g.endWrite();
    dirty = false;
}

}  // namespace

bool open() { return is_open; }

const char* name() { return is_open ? overlay_name(current.kind) : ""; }

void show(Overlay incoming) {
    current = std::move(incoming);
    is_open = dirty = true;
    set_overlay_kind(overlay_name(current.kind));
}

void draw(bool force) {
    if (dirty || (force && current.kind != Overlay::Kind::Image)) draw_overlay();
}

void dismiss() {
    is_open = false;
    current = Overlay();
    set_overlay_kind("");
}

}  // namespace vat::ui::overlay
