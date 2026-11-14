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
#include "gfx_exec.h"
#include "ui_compose.h"
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
    static compose::PerUpdate<std::vector<String>> wrapped;  // once for all the bands
    const auto& parts = wrapped.get(g, [&] {
        std::vector<String> out;
        for (const auto& line : current.lines) {
            for (auto& part : wrap(g, line, kWidth - kPad * 2)) out.push_back(std::move(part));
        }
        return out;
    });
    for (const auto& part : parts) {
        if (y + kLine > text_bottom) break;
        if (compose::visible(g, y, kLine)) g.drawString(part, kPad, y);
        y += kLine;
    }
    draw_choices(g);
    footer(g, {choice_rows ? "; . 选择  Enter 确定  ` 关闭" : "` 关闭"});
}

// A canvas is composed band by band like everything else (ui_compose): the
// ops of a frame are prepared once (variables stepped, "{t}" filled) and run
// for each band.
std::vector<ui_canvas::Op> frame_ops;
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

// This frame's ops: the update run once, its values filled in.
void prepare_canvas() {
    frame_ops = current.canvas;
    const long t = static_cast<long>(anim_frame) * current.step;
    if (canvas_script.active()) {
        step_vars(t);
        ui_canvas::fill_vars(frame_ops, [](const String& name, String& value) { return canvas_script.text(name, value); });
    } else if (current.frame_ms) {
        ui_canvas::fill_frame(frame_ops, t);
    }
}

void draw_canvas(Gfx& g) {
    gfx_exec::reset(g);  // every band starts the frame from the same state
    gfx_exec::Picture picture;
    if (!current.image.empty()) picture = {&current.image, current.image_x, current.image_y};
    gfx_exec::run(g, frame_ops, current.blobs, picture);
    if (!update_error.isEmpty()) {  // the update stopped: say why on the canvas
        g.setFont(&cjk::cn12);
        g.setTextSize(1);
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(kWarn, kBg);
        g.drawString(utf8_head(update_error, 36), 2, kHeight - 14);
    }
}

void paint_overlay(Gfx& g) {
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
        struct Fitted {
            int font = 0;
            std::vector<String> lines;
        };
        static compose::PerUpdate<Fitted> fitted;  // the size and the lines, once for all the bands
        const Fitted& fit_text = fitted.get(g, [&] {
            Fitted out;
            for (out.font = 0; out.font < 3; ++out.font) {
                g.setFont(fonts_by_size[out.font]);
                g.setTextSize(scales[out.font]);
                out.lines = wrap(g, current.text, kWidth - kPad * 2);
                if (static_cast<int>(out.lines.size()) * heights[out.font] <= kHeight - kLine) break;
            }
            if (out.font == 3) out.font = 2;
            return out;
        });
        const int font = fit_text.font;
        const auto& lines = fit_text.lines;
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
        footer(g, {"任意键返回"});
    } else if (current.kind == Overlay::Kind::Qr) {
        const int x = qr_panel(g, current.text);
        g.setFont(&cjk::cn12);
        g.setTextColor(kText, kBg);
        int y = 4;
        for (const auto& line : wrap(g, current.text, kWidth - x - 2)) {
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
            g.setFont(&cjk::cn12);
            g.setTextColor(kWarn, kBg);
            g.drawCentreString("图片无法解码", kWidth / 2, kHeight / 2 - 6);
        }
    }
}

void draw_overlay() {
    if (current.kind == Overlay::Kind::Canvas) prepare_canvas();
    if (current.kind == Overlay::Kind::Image && current.image.empty() && !current.image_path.isEmpty()) {
        storage::read_bytes(current.image_path, current.image, 32768);  // drawn again: from the card
    }
    compose::frame(paint_overlay);
    std::vector<ui_canvas::Op>().swap(frame_ops);  // made again for the next frame
    if (current.kind == Overlay::Kind::Image) {  // drawn; the screen keeps it
        current.image.clear();
        current.image.shrink_to_fit();
    }
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
        anim_frame = 0;
        anim_last = millis();
        start_vars();
    }
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

void suspend() {}

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
        events::emit("ui://event/panel/" + current.panel, json);
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
        const int n = static_cast<int>(current.choices.size());
        compose::area({0, kHeight - kLine - n * kLine - 1, kWidth, n * kLine}, draw_choices);
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
    drop_vars();
    is_open = false;
    current = Overlay();
    set_overlay_kind("");
}

}  // namespace vat::ui::overlay
