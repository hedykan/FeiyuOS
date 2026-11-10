#include "ui_canvas.h"
#include "cjk_font.h"
#include "arena.h"
#include "ui_overlay.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <cmath>

#include "common.h"
#include "events.h"
#include "lua_vm.h"
#include "registry.h"
#include "storage.h"
#include "ui_kit.h"

namespace vat::ui::overlay {
namespace {

Overlay current;
bool is_open = false;
bool dirty = false;
int cursor = 0;  // panel choice
CursorSlide choice_bar;  // the highlight of the panel's choices
bool choices_dirty = false;  // only the cursor moved: redraw the choice rows

const char* overlay_name(Overlay::Kind kind) {
    switch (kind) {
        case Overlay::Kind::Text: return "text";
        case Overlay::Kind::Qr: return "qr";
        case Overlay::Kind::Image: return "image";
        case Overlay::Kind::Panel: return "panel";
        case Overlay::Kind::Canvas: return "canvas";
        default: return "";
    }
}

// The choice rows, bottom-up above the footer, each through the strip so the
// sliding highlight (between two rows while it moves) never flickers.
void draw_choices(Gfx& g) {
    const int n = static_cast<int>(current.choices.size());
    if (!n) return;
    const int top = kHeight - kLine - n * kLine;
    const int bar = choice_bar.follow(top + cursor * kLine - 1, n);
    for (int i = 0; i < n; ++i) {
        const int y = top + i * kLine;
        const bool on = i == cursor;
        band(g, y - 1, kLine, kBg, [&](Gfx& b, int t) {
            const int offset = bar - (y - 1);
            if (offset > -kLine && offset < kLine) {
                b.fillRect(0, t + offset, kWidth, kLine, kSurface);
                b.fillRect(0, t + offset, 2, kLine, kAccent);
            }
            b.setTextColor(on ? kAccent : kText);  // transparent: the bar shows behind the text
            b.drawString(String(on ? "> " : "  ") + utf8_head(current.choices[i], 90), kPad, t + 1);
        });
    }
}

void draw_panel(Gfx& g) {
    g.setFont(&cjk::cn12);
    int y = 2;
    if (!current.title.isEmpty()) {
        g.setTextColor(kAccent, kBg);
        g.drawString(utf8_head(current.title, 60), kPad, y);
        y += kLine + 2;
    }
    const int footer_y = kHeight - kLine;
    const int choice_rows = static_cast<int>(current.choices.size());
    const int text_bottom = footer_y - choice_rows * kLine - (choice_rows ? 2 : 0);
    g.setTextColor(kText, kBg);
    for (const auto& line : current.lines) {
        for (const auto& part : wrap_text(g, line, kWidth - kPad * 2)) {
            if (y + kLine > text_bottom) break;
            g.drawString(part, kPad, y);
            y += kLine;
        }
    }
    draw_choices(g);
    g.setTextColor(kMuted, kBg);
    g.drawRightString(choice_rows ? "; . 选择  Enter 确定  ` 关闭" : "` 关闭", kWidth - kPad, footer_y);
}

uint16_t rgb565(const String& text, uint16_t fallback) {
    uint32_t rgb;
    if (!ui_canvas::color(text, rgb)) return fallback;
    return lgfx::color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

textdatum_t datum_of(const String& name) {
    static const char* const names[] = {"top_left", "top_center", "top_right", "middle_left", "middle_center",
                                        "middle_right", "bottom_left", "bottom_center", "bottom_right"};
    static const textdatum_t values[] = {textdatum_t::top_left,    textdatum_t::top_center,    textdatum_t::top_right,
                                         textdatum_t::middle_left, textdatum_t::middle_center, textdatum_t::middle_right,
                                         textdatum_t::bottom_left, textdatum_t::bottom_center, textdatum_t::bottom_right};
    for (int i = 0; i < 9; ++i) {
        if (name == names[i]) return values[i];
    }
    return textdatum_t::top_left;
}

// Animated canvases draw into an 8-bit frame (32 KB leased from the arena)
// pushed in one go, so a redraw never shows a blank screen; without the
// lease they draw straight to the display.
LGFX_Sprite* frame = nullptr;
bool frame_refused = false;  // the arena said no for this canvas: draw on screen, do not ask every frame
uint32_t anim_frame = 0;
// Canvas variables: live values, the parsed update, and its first error
// (shown in the corner; the update then stops).
luavm::Script canvas_script;  // vars + update, while the canvas is open
String update_error;
char shown_error[96] = "";  // update_error for the web task (plain bytes, no String)

void drop_vars() {
    canvas_script.close();
    update_error = String();
    shown_error[0] = 0;
}

void start_vars() {
    drop_vars();
    if (current.kind != Overlay::Kind::Canvas || (current.vars_json.isEmpty() && current.update.isEmpty())) return;
    update_error = canvas_script.open(current.vars_json.isEmpty() ? String("{}") : current.vars_json, current.update);
    if (!update_error.isEmpty()) strlcpy(shown_error, utf8_head(update_error, 90).c_str(), sizeof(shown_error));
}

// One frame of the update; t = frame × step.
void step_vars(long t) {
    if (!canvas_script.active() || !update_error.isEmpty()) return;
    update_error = canvas_script.step(t);
    if (!update_error.isEmpty()) strlcpy(shown_error, utf8_head(update_error, 90).c_str(), sizeof(shown_error));
}
unsigned long anim_last = 0;

void drop_frame() {
    if (!frame) return;
    frame->deleteSprite();
    delete frame;
    frame = nullptr;
    arena::give_back("canvas_frame");
}

bool make_frame() {
    if (frame) return true;
    if (!arena::take("canvas_frame", kWidth * kHeight, true)) return false;
    frame = new LGFX_Sprite(&M5Cardputer.Display);
    frame->setColorDepth(8);
    if (!frame->createSprite(kWidth, kHeight)) {
        delete frame;
        frame = nullptr;
        arena::give_back("canvas_frame");
        return false;
    }
    return true;
}

struct Turn {
    bool on = false;
    int angle = 0, cx = 0, cy = 0;
    void at(int& x, int& y) const {
        if (on) ui_canvas::rotate_point(angle, cx, cy, x, y);
    }
};

void polygon(lgfx::LGFXBase& g, const int* xs, const int* ys, int n, uint16_t c, bool fill) {
    if (fill) {
        for (int i = 1; i + 1 < n; ++i) g.fillTriangle(xs[0], ys[0], xs[i], ys[i], xs[i + 1], ys[i + 1], c);
    } else {
        for (int i = 0; i < n; ++i) g.drawLine(xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], c);
    }
}

void turned_rect(lgfx::LGFXBase& g, const Turn& t, int x, int y, int w, int h, uint16_t c, bool fill) {
    int xs[4] = {x, x + w - 1, x + w - 1, x}, ys[4] = {y, y, y + h - 1, y + h - 1};
    for (int i = 0; i < 4; ++i) t.at(xs[i], ys[i]);
    polygon(g, xs, ys, 4, c, fill);
}

void turned_ellipse(lgfx::LGFXBase& g, const Turn& t, int cx, int cy, int rx, int ry, uint16_t c, bool fill) {
    constexpr int kSteps = 36;
    int xs[kSteps], ys[kSteps];
    for (int i = 0; i < kSteps; ++i) {
        const float a = 6.2831853f * i / kSteps;
        xs[i] = cx + static_cast<int>(std::lround(rx * std::cos(a)));
        ys[i] = cy + static_cast<int>(std::lround(ry * std::sin(a)));
        t.at(xs[i], ys[i]);
    }
    polygon(g, xs, ys, kSteps, c, fill);
}

void draw_canvas_on(lgfx::LGFXBase& g) {
    using ui_canvas::Kind;
    std::vector<ui_canvas::Op> ops = current.canvas;
    const long t = static_cast<long>(anim_frame) * current.step;
    if (canvas_script.active()) {
        step_vars(t);
        ui_canvas::fill_vars(ops, [](const String& name, String& value) { return canvas_script.text(name, value); });
    } else if (current.frame_ms) {
        ui_canvas::fill_frame(ops, t);
    }
    g.fillScreen(kBg);
    Turn turn;
    for (const auto& op : ops) {
        int v[8] = {};
        for (std::size_t i = 0; i < op.args.size() && i < 8; ++i) ui_canvas::number(op.args[i], v[i]);
        const uint16_t c = rgb565(op.color, kText);
        switch (op.kind) {
            case Kind::Rotate:
                turn.on = v[0] % 360 != 0;
                turn.angle = v[0];
                turn.cx = v[1];
                turn.cy = v[2];
                break;
            case Kind::FillScreen: g.fillScreen(c); break;
            case Kind::DrawPixel: turn.at(v[0], v[1]); g.drawPixel(v[0], v[1], c); break;
            case Kind::DrawLine: turn.at(v[0], v[1]); turn.at(v[2], v[3]); g.drawLine(v[0], v[1], v[2], v[3], c); break;
            case Kind::DrawFastHLine:
                if (turn.on) { int x1 = v[0] + v[2] - 1, y1 = v[1]; turn.at(v[0], v[1]); turn.at(x1, y1); g.drawLine(v[0], v[1], x1, y1, c); }
                else g.drawFastHLine(v[0], v[1], v[2], c);
                break;
            case Kind::DrawFastVLine:
                if (turn.on) { int x1 = v[0], y1 = v[1] + v[2] - 1; turn.at(v[0], v[1]); turn.at(x1, y1); g.drawLine(v[0], v[1], x1, y1, c); }
                else g.drawFastVLine(v[0], v[1], v[2], c);
                break;
            case Kind::DrawRect:
            case Kind::DrawRoundRect:
                if (turn.on) turned_rect(g, turn, v[0], v[1], v[2], v[3], c, false);
                else if (op.kind == Kind::DrawRect) g.drawRect(v[0], v[1], v[2], v[3], c);
                else g.drawRoundRect(v[0], v[1], v[2], v[3], v[4], c);
                break;
            case Kind::FillRect:
            case Kind::FillRoundRect:
                if (turn.on) turned_rect(g, turn, v[0], v[1], v[2], v[3], c, true);
                else if (op.kind == Kind::FillRect) g.fillRect(v[0], v[1], v[2], v[3], c);
                else g.fillRoundRect(v[0], v[1], v[2], v[3], v[4], c);
                break;
            case Kind::DrawCircle: turn.at(v[0], v[1]); g.drawCircle(v[0], v[1], v[2], c); break;
            case Kind::FillCircle: turn.at(v[0], v[1]); g.fillCircle(v[0], v[1], v[2], c); break;
            case Kind::DrawEllipse:
            case Kind::FillEllipse: {
                const bool fill = op.kind == Kind::FillEllipse;
                if (turn.on) turned_ellipse(g, turn, v[0], v[1], v[2], v[3], c, fill);
                else if (fill) g.fillEllipse(v[0], v[1], v[2], v[3], c);
                else g.drawEllipse(v[0], v[1], v[2], v[3], c);
                break;
            }
            case Kind::DrawTriangle:
            case Kind::FillTriangle:
                turn.at(v[0], v[1]); turn.at(v[2], v[3]); turn.at(v[4], v[5]);
                if (op.kind == Kind::FillTriangle) g.fillTriangle(v[0], v[1], v[2], v[3], v[4], v[5], c);
                else g.drawTriangle(v[0], v[1], v[2], v[3], v[4], v[5], c);
                break;
            case Kind::DrawArc:
            case Kind::FillArc: {
                turn.at(v[0], v[1]);
                const int extra = turn.on ? turn.angle : 0;  // an arc turns by moving its angles
                if (op.kind == Kind::FillArc) g.fillArc(v[0], v[1], v[2], v[3], v[4] + extra, v[5] + extra, c);
                else g.drawArc(v[0], v[1], v[2], v[3], v[4] + extra, v[5] + extra, c);
                break;
            }
            case Kind::DrawBezier: turn.at(v[0], v[1]); turn.at(v[2], v[3]); turn.at(v[4], v[5]); g.drawBezier(v[0], v[1], v[2], v[3], v[4], v[5], c); break;
            case Kind::DrawGradientLine: turn.at(v[0], v[1]); turn.at(v[2], v[3]); g.drawGradientLine(v[0], v[1], v[2], v[3], c, rgb565(op.color2, c)); break;
            case Kind::DrawString: {
                int x = 0, y = 0;
                ui_canvas::number(op.args[1], x);
                ui_canvas::number(op.args[2], y);
                turn.at(x, y);  // text moves with the turn; the glyphs stay upright
                g.setFont(&cjk::cn12);
                g.setTextSize(op.size);
                g.setTextDatum(datum_of(op.datum));
                g.setTextColor(c);
                g.drawString(op.args[0], x, y);
                g.setTextDatum(textdatum_t::top_left);
                g.setTextSize(1);
                break;
            }
            case Kind::DrawQr: {
                int x = 0, y = 0, w = 64;
                ui_canvas::number(op.args[1], x);
                ui_canvas::number(op.args[2], y);
                ui_canvas::number(op.args[3], w);
                turn.at(x, y);
                g.qrcode(op.args[0].c_str(), x, y, std::clamp(w, 21, kHeight));
                break;
            }
            case Kind::DrawImage: {
                const auto& b = current.image;
                int x = current.image_x, y = current.image_y;
                turn.at(x, y);
                if (b.size() > 4 && b[0] == 0x89 && b[1] == 'P') g.drawPng(b.data(), b.size(), x, y);
                else if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8) g.drawJpg(b.data(), b.size(), x, y);
                else if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') g.drawBmp(b.data(), b.size(), x, y);
                break;
            }
            case Kind::ProgressBar: {
                const int value = std::clamp(v[4], 0, 100);
                if (turn.on) {
                    turned_rect(g, turn, v[0], v[1], v[2], v[3], rgb565(op.color2, kSurface), true);
                    if (value) turned_rect(g, turn, v[0], v[1], v[2] * value / 100, v[3], c, true);
                } else {
                    g.fillRect(v[0], v[1], v[2], v[3], rgb565(op.color2, kSurface));
                    g.fillRect(v[0], v[1], v[2] * value / 100, v[3], c);
                }
                break;
            }
        }
    }
    if (!update_error.isEmpty()) {  // the update stopped: say why on the canvas
        g.setFont(&cjk::cn12);
        g.setTextColor(kWarn, kBg);
        g.drawString(utf8_head(update_error, 36), 2, kHeight - 14);
    }
}

// Every canvas is drawn in memory and pushed whole (drawing op by op on the
// display shows the shapes appear one after another).  A still canvas gives
// the frame back at once; an animated one keeps it until it closes.
void draw_canvas(Gfx& g) {
    if (!frame_refused && make_frame()) {
        draw_canvas_on(*frame);
        frame->pushSprite(0, 0);
        if (!current.frame_ms) drop_frame();
    } else {
        frame_refused = true;
        draw_canvas_on(g);
    }
}

void draw_overlay() {
    auto& g = M5Cardputer.Display;
    g.startWrite();
    g.fillScreen(kBg);
    if (current.kind == Overlay::Kind::Canvas) {
        draw_canvas(g);
    } else if (current.kind == Overlay::Kind::Panel) {
        draw_panel(g);
    } else if (current.kind == Overlay::Kind::Text) {
        // Largest size that fits the whole text.  Only fonts already linked
        // for the terminal: another CJK font costs ~450 KB of flash.
        static const lgfx::IFont* const fonts_by_size[] = {&cjk::cn12, &cjk::cn14, &cjk::cn12};
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
        g.setFont(&cjk::cn12);
        g.setTextColor(kMuted, kBg);
        g.drawRightString("任意键返回", kWidth - kPad, kHeight - kLine);
    } else if (current.kind == Overlay::Kind::Qr) {
        const int x = qr_panel(g, current.text);
        g.setFont(&cjk::cn12);
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
        if (current.image.empty() && !current.image_path.isEmpty()) {
            storage::read_bytes(current.image_path, current.image, 32768);  // drawn again: from the card
        }
        const auto& b = current.image;
        const auto datum = textdatum_t::middle_center;
        bool ok = false;
        if (b.size() > 4 && b[0] == 0x89 && b[1] == 'P') ok = g.drawPng(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        else if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8) ok = g.drawJpg(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        else if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') ok = g.drawBmp(b.data(), b.size(), 0, 0, kWidth, kHeight, 0, 0, 0.0f, 0.0f, datum);
        if (!ok) {
            g.setFont(&cjk::cn12);
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

uint32_t canvas_frame() { return is_open && current.kind == Overlay::Kind::Canvas ? anim_frame : 0; }

const char* canvas_error() { return shown_error; }

const char* name() { return is_open ? overlay_name(current.kind) : ""; }

bool show(Overlay incoming) {
    // A refresh of the panel on screen keeps the cursor.
    const bool same_panel = is_open && current.kind == incoming.kind &&
                            (current.kind == Overlay::Kind::Panel || current.kind == Overlay::Kind::Canvas) &&
                            current.panel == incoming.panel;
    current = std::move(incoming);
    if (current.kind == Overlay::Kind::Canvas && !current.image.empty()) arena::note("canvas_image", current.image.size());
    else arena::give_back("canvas_image");
    if (!same_panel) {  // a refresh of the same canvas keeps its frame count
        frame_refused = false;  // a new canvas asks for memory again
        anim_frame = 0;
        anim_last = millis();
        start_vars();
    }
    if (current.kind != Overlay::Kind::Canvas || !current.frame_ms) drop_frame();
    if (!same_panel) {
        cursor = 0;
        choice_bar.reset();  // a new panel: the highlight starts in place
    }
    cursor = std::min<int>(cursor, std::max<int>(0, static_cast<int>(current.choices.size()) - 1));
    is_open = dirty = true;
    set_overlay_kind(current.kind == Overlay::Kind::Panel    ? "panel:" + current.panel
                     : current.kind == Overlay::Kind::Canvas ? "canvas:" + current.panel
                                                             : String(overlay_name(current.kind)));
    return !same_panel;
}

void suspend() { drop_frame(); }

bool is_panel() { return is_open && current.kind == Overlay::Kind::Panel; }

bool panel_key(const Keyboard_Class::KeysState& state) {
    const auto has = [&state](char c) {
        for (const char k : state.word) {
            if (k == c) return true;
        }
        return false;
    };
    const int n = static_cast<int>(current.choices.size());
    if (has('`')) return true;
    if (n && has(';')) cursor = (cursor + n - 1) % n;
    else if (n && has('.')) cursor = (cursor + 1) % n;
    else if (n && state.enter) {
        const String choice = current.choices[cursor];
        set_panel_choice(current.panel, choice);
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "choice", choice.c_str());
        cJSON_AddNumberToObject(data, "index", cursor + 1);
        const String json = json_print(data);
        cJSON_Delete(data);
        events::emit("ui://panel/" + current.panel, json);
        return true;
    } else {
        return false;
    }
    choices_dirty = true;
    return false;
}

void draw(bool force) {
    if (is_open && current.kind == Overlay::Kind::Canvas && current.frame_ms &&
        (current.frames == 0 || anim_frame + 1 < current.frames) && millis() - anim_last >= current.frame_ms) {
        ++anim_frame;
        anim_last = millis();
        dirty = true;
    }
    if (!dirty && !force && is_open && current.kind == Overlay::Kind::Panel && (choices_dirty || choice_bar.sliding())) {
        auto& g = M5Cardputer.Display;
        g.startWrite();
        draw_choices(g);
        g.endWrite();
        choices_dirty = false;
        return;
    }
    if (dirty || force) {
        choices_dirty = false;
        draw_overlay();
    }
}

void dismiss() {
    arena::give_back("canvas_image");
    drop_frame();
    drop_vars();
    is_open = false;
    current = Overlay();
    set_overlay_kind("");
}

}  // namespace vat::ui::overlay
