#include <new>
#include "events.h"
#include "cjk_font.h"
#include "ui.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <math.h>

#include "agent.h"
#include "common.h"
#include "config.h"
#include "console.h"
#include "net.h"
#include "ops_local.h"
#include "ops_tui.h"
#include "ops_hw.h"
#include "ops_timer.h"
#include "ops_wifi.h"
#include "prog_runtime.h"
#include "registry.h"
#include "storage.h"
#include "settings_page.h"
#include "ui_kit.h"
#include "ui_mascot.h"
#include "ui_overlay.h"
#include "ui_recorder.h"
#include "ui_status.h"
#include "worker.h"

namespace vat::ui {
namespace {


constexpr uint32_t kScreenTimeouts[] = {0, 30, 60, 180, 300};
constexpr unsigned long kTerminalRefreshMs = 5000;

enum class View : uint8_t { List, Detail, Help };
enum class VoiceMode : uint8_t { Agent, Text };

Page current_page = Page::Agent;
Page previous_page = Page::Agent;  // where closing the graphics page returns to
bool input_from_graphics = false;   // a program asked for a line while its picture was up
View view = View::List;
// The program page: the list (programs and local apps) or the console.
struct ProgramItem {
    String name;
    String about;
    bool app = false;  // a local app (ui://apps): opens on the graphics page
    time_t used = 0;   // last run / saved / changed: the list is newest first
};
int delete_pending = -1;   // the item Del asked about (Y deletes it), -1: none
// The list is cached: reading every program's header from the card takes a
// moment, so entering the page draws the cache and refreshes it a frame later,
// only when something changed (programs, a run, or a minute went by).
bool program_items_loaded = false;
bool program_items_stale = true;
uint32_t program_items_at = 0;
uint32_t program_items_gen = 0;
uint32_t program_items_run = 0;
std::vector<ProgramItem> program_items;
bool program_list = true;  // false: the console (a run, or the last one's output)
int program_cursor = 0;
int output_scroll = 0;     // display rows up from the newest
uint32_t console_drawn = UINT32_MAX;
int program_layout = -1;   // what the program page shows (list / console); -1: clear it first
uint32_t console_second = 0;
CursorSlide program_bar;
CursorSlide program_scroll;
CursorSlide help_bar;
CursorSlide help_list_scroll;
CursorSlide timeline_scroll;  // the timeline's first row × kLine
VoiceMode voice_mode = VoiceMode::Agent;
int cursor = -1;  // index into the timeline; -1 follows the newest entry
int detail_scroll = 0;
uint32_t detail_id = 0;
std::vector<std::pair<String, uint16_t>> detail_lines;
int help_page = 0;
int help_cursor = 0;     // row on pages 2 and 3
bool help_detail = false;  // the row's full help is open
int help_scroll = 0;
bool screen_on = true;
unsigned long last_activity = 0;
uint32_t drawn_version = UINT32_MAX;
unsigned long last_draw = 0;
unsigned long last_terminal_refresh = 0;
bool mascot_live = false;  // the corner mascot is on screen and animating

// Masked input for secrets requested by a paused step.
String secret_buffer;
bool secret_reveal = false;

// Setup page (Wi-Fi from the keyboard).


// ---------------------------------------------------------------- text helpers

std::vector<String> wrap(Gfx& g, const String& text, int max_px, int max_lines) {
    std::vector<String> lines;
    int start = 0;
    while (start <= static_cast<int>(text.length()) && static_cast<int>(lines.size()) < max_lines) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = text.length();
        String paragraph = text.substring(start, end);
        start = end + 1;
        if (paragraph.isEmpty()) {
            if (start <= static_cast<int>(text.length())) lines.push_back("");
            continue;
        }
        while (!paragraph.isEmpty() && static_cast<int>(lines.size()) < max_lines) {
            String line;
            std::size_t i = 0;
            while (i < paragraph.length()) {
                std::size_t len = 1;
                const unsigned char c = paragraph[i];
                if (c >= 0xF0) len = 4;
                else if (c >= 0xE0) len = 3;
                else if (c >= 0xC0) len = 2;
                const String next = line + paragraph.substring(i, i + len);
                if (g.textWidth(next) > max_px && !line.isEmpty()) break;
                line = next;
                i += len;
            }
            lines.push_back(line);
            paragraph = paragraph.substring(i);
        }
    }
    if (static_cast<int>(lines.size()) == max_lines && start < static_cast<int>(text.length())) {
        lines.back() = fit(g, lines.back() + "…", max_px);
    }
    return lines;
}

// ---------------------------------------------------------------- primitives

void icon(Gfx& g, int x, int y, Status status, bool selected) {
    const uint16_t bg = selected ? kSurface : kBg;
    switch (status) {
        case Status::Done:
            g.drawLine(x, y + 4, x + 3, y + 7, kAccent);
            g.drawLine(x + 3, y + 7, x + 8, y + 1, kAccent);
            g.drawLine(x, y + 5, x + 3, y + 8, kAccent);
            g.drawLine(x + 3, y + 8, x + 8, y + 2, kAccent);
            break;
        case Status::Failed:
            g.drawLine(x, y + 1, x + 7, y + 8, kDanger);
            g.drawLine(x + 7, y + 1, x, y + 8, kDanger);
            g.drawLine(x + 1, y + 1, x + 8, y + 8, kDanger);
            g.drawLine(x + 8, y + 1, x + 1, y + 8, kDanger);
            break;
        case Status::Confirm:
        case Status::Secret:
            g.fillCircle(x + 4, y + 5, 4, kWarn);
            g.setTextColor(bg, kWarn);
            g.setFont(&fonts::Font0);
            g.drawString(status == Status::Confirm ? "?" : "*", x + 2, y + 2);
            g.setFont(&cjk::cn12);
            break;
        case Status::Cancelled:
            g.drawFastHLine(x + 1, y + 5, 7, kMuted);
            break;
        case Status::Text:
            g.drawRect(x, y + 1, 9, 9, kMuted);
            break;
        case Status::Running: {
            const int phase = (millis() / 150) % 8;
            for (int i = 0; i < 8; ++i) {
                const float a = i * 0.785f;
                const int px = x + 4 + static_cast<int>(3.5f * cosf(a));
                const int py = y + 5 + static_cast<int>(3.5f * sinf(a));
                g.drawPixel(px, py, i == phase ? kAccent : kMuted);
            }
            break;
        }
    }
}

const char* page_name(Page page) {
    switch (page) {
        case Page::Agent: return "Agent";
        case Page::Terminal: return "SSH";
        case Page::Program: return "程序";
        default: return "图形";
    }
}

void top_bar(Gfx& screen, const Model& model) {
    // Through the strip: the bar is also redrawn on its own when a status item
    // changes (the minute, the battery), without flashing.
    band(screen, 0, kBarHeight, kSurface, [&](Gfx& g, int top) {
        const char* name = page_name(current_page);
        g.setTextColor(kText, kSurface);
        g.drawString(name, kPad, top + 2);
        int left = kPad + g.textWidth(name) + 10;
        if (current_page == Page::Agent) {
            // The host is the SSH page's business; here the chosen status items.
            if (model.busy) {
                const String secs = String((millis() - model.stage_started_ms) / 1000) + "s";
                g.setTextColor(kAccent, kSurface);
                g.drawString(secs, left, top + 2);
                left += g.textWidth(secs) + 6;
            }
            status::draw(g, top, kWidth - kPad, left, kSurface);
            return;
        }
        if (current_page == Page::Program) {
            const console::Head h = console::head();
            if (h.name.isEmpty() || program_list) return;
            String right;
            uint16_t color = kMuted;
            if (h.running) {
                right = String((millis() - h.started) / 1000) + "s";
                color = kAccent;
            } else {
                right = h.status;
                color = h.failed ? kDanger : kMuted;
            }
            right = fit(g, right, 90);
            g.setTextColor(color, kSurface);
            g.drawRightString(right, kWidth - kPad, top + 2);
            g.setTextColor(kText, kSurface);
            g.drawString(fit(g, h.name, kWidth - kPad - g.textWidth(right) - 8 - left), left, top + 2);
            return;
        }
        int right = kWidth - kPad - 12;
        wifi_bars(g, right, top + 3);
        right -= 6;
        String where = model.terminal_title;
        const bool online = where != "未连接";
        g.fillCircle(right - 3, top + 7, 3, online ? kAccent : kMuted);
        right -= 10;
        g.setTextColor(online ? kText : kMuted, kSurface);
        where = fit(g, where, std::min(90, right - left));
        g.drawString(where, right - g.textWidth(where), top + 2);
    });
}

// ---------------------------------------------------------------- Agent timeline

// 识别 · 理解 · 执行 · 总结: done = filled accent dot, current = blinking
// ring, pending = muted ring.  Drawn, not glyphs, so no font gaps.
// The only thing that moves while a task runs is the dot inside the current
// stage's ring.  Its position is kept so animation frames redraw just that
// dot: redrawing the whole screen for it (no off-screen buffer) flickered
// about three times a second.
struct StageDot {
    bool valid = false;
    int x = 0;
    int y = 0;
    uint16_t bg = 0;
} stage_dot;

void draw_stage(Gfx& g, int x, int y, const Entry& entry, uint16_t bg) {
    const String stage = entry.stage;
    int phase = 1;
    if (stage.startsWith("STT")) phase = 0;
    else if (stage.startsWith("RUN") || stage.startsWith("STEP")) phase = 2;
    else if (stage.startsWith("SUMMARY")) phase = 3;
    const char* names[] = {"识别", "理解", "执行", "总结"};
    for (int i = 0; i < 4; ++i) {
        const int cy = y + 6;
        if (i < phase) g.fillCircle(x + 3, cy, 3, kAccent);
        else if (i == phase) {
            g.drawCircle(x + 3, cy, 3, kAccent);
            if ((millis() / 400) % 2) g.fillCircle(x + 3, cy, 1, kAccent);
            stage_dot = {true, x + 3, cy, bg};
        } else g.drawCircle(x + 3, cy, 3, kMuted);
        x += 9;
        String label = names[i];
        if (i == 2 && entry.step > 1) label += String(entry.step);
        g.setTextColor(i <= phase ? kText : kMuted, bg);
        g.drawString(label, x, y);
        x += g.textWidth(label) + 8;
    }
}

struct Row {
    String text;
    uint16_t color;
    int entry;       // timeline index this row belongs to
    bool head;       // first row of the entry (time + text + icon)
    int indent;
};

std::vector<Row> timeline_rows(Gfx& g, const Model& model, int selected) {
    std::vector<Row> rows;
    const int text_w = kWidth - kPad * 2 - 44 - 14;
    for (int i = 0; i < static_cast<int>(model.timeline.size()); ++i) {
        const Entry& e = model.timeline[i];
        rows.push_back({fit(g, e.text, text_w), kText, i, true, 0});
        const bool is_last = i == static_cast<int>(model.timeline.size()) - 1;
        const int inner = kWidth - kPad * 2 - 44;
        if (e.status == Status::Running && is_last) {
            rows.push_back({"\x01stage", kAccent, i, false, 44});
        } else if (e.status == Status::Confirm && is_last && !model.pending.empty()) {
            const auto& p = model.pending.front();
            const String host = p.summary.indexOf("ssh://") >= 0
                                    ? p.summary.substring(p.summary.indexOf("ssh://") + 6)
                                    : String("本机");
            String host_name = host;
            const int cut = host_name.indexOf(' ');
            if (cut > 0) host_name = host_name.substring(0, cut);
            const int slash = host_name.indexOf('/');
            if (slash > 0) host_name = host_name.substring(0, slash);
            rows.push_back({"需要确认 · " + host_name + (p.step ? "  步骤 " + String(p.step) : String()), kWarn, i,
                            false, 44});
            String command = p.summary;
            const int dollar = command.indexOf("$ ");
            if (dollar >= 0) command = command.substring(dollar);
            for (const auto& line : wrap(g, command, inner, 2)) rows.push_back({line, kText, i, false, 44});
            rows.push_back({"Y 执行   N 取消   A 后续都执行", kMuted, i, false, 44});
        } else if (e.status == Status::Secret && is_last) {
            rows.push_back({"需要输入密码", kWarn, i, false, 44});
            rows.push_back({fit(g, e.summary, inner), kMuted, i, false, 44});
        } else if (i == selected && !e.summary.isEmpty()) {
            const uint16_t color = e.status == Status::Failed ? kDanger : kText;
            for (const auto& line : wrap(g, e.summary, inner, 2)) rows.push_back({line, color, i, false, 44});
        }
    }
    return rows;
}

// The selected entry's highlight: its top and bottom edges slide on their own,
// so it changes height as it moves to an entry of more or fewer rows.
CursorSlide timeline_top;
CursorSlide timeline_bottom;
bool timeline_sliding() { return timeline_top.sliding() || timeline_bottom.sliding() || timeline_scroll.sliding(); }

void draw_timeline(Gfx& g, const Model& model, bool mascot_on) {
    g.setFont(&cjk::cn12);
    const int count = model.timeline.size();
    const int selected = cursor < 0 || cursor >= count ? count - 1 : cursor;
    const auto rows = timeline_rows(g, model, selected);
    const int top = kBarHeight + 2;
    const int footer = (!model.busy && model.pending.empty()) || !model.notice.isEmpty() ? kLine : 0;
    const int visible = (kHeight - top - footer) / kLine;

    // Keep the selected entry's block in view (bottom-anchored like a chat).
    int last_row = rows.size() - 1;
    for (int r = rows.size() - 1; r >= 0; --r) {
        if (rows[r].entry == selected) {
            last_row = r;
            break;
        }
    }
    int first = std::max(0, last_row - visible + 1);
    if (cursor >= 0) {
        int head = 0;
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            if (rows[r].entry == selected && rows[r].head) head = r;
        }
        first = std::min(first, head);
    }
    const int scroll_px = timeline_scroll.follow(first * kLine, 0);  // the rows slide to `first`
    const int region_top = top - 1;
    const int region_bottom = top - 1 + visible * kLine;

    // The highlight [bar_top, bar_bottom): the selected entry's rows, sliding there.
    int bar_top = -1;
    int bar_bottom = -1;
    if (cursor >= 0) {
        int head = -1;
        int tail = -1;
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            if (rows[r].entry != selected) continue;
            if (head < 0) head = r;
            tail = r;
        }
        if (head >= 0) {
            // In the timeline's own coordinates, so a scroll does not make it jump.
            bar_top = timeline_top.follow(top - 1 + head * kLine, 0) - scroll_px;
            bar_bottom = timeline_bottom.follow(top - 1 + (tail + 1) * kLine, 0) - scroll_px;
        }
    } else {
        timeline_top.reset();
        timeline_bottom.reset();
    }

    g.fillRect(0, kBarHeight, kWidth, top - 1 - kBarHeight, kBg);
    if (mascot_on) mascot::reset_backdrop(kBg);  // rows beside it set their own colour below
    g.setClipRect(0, region_top, kWidth, region_bottom - region_top);  // rows half out while sliding
    for (int r = std::max(0, scroll_px / kLine); r < static_cast<int>(rows.size()); ++r) {
        const Row& row = rows[r];
        const int y = top + r * kLine - scroll_px;
        if (y - 1 >= region_bottom) break;
        const Entry& e = model.timeline[row.entry];
        const bool sel = row.entry == selected && cursor >= 0;
        const bool attention = row.entry == count - 1 &&
                               (e.status == Status::Confirm || e.status == Status::Secret);
        // The part of this row under the (maybe sliding) highlight.
        const int hl_top = std::max(bar_top, y - 1);
        const int hl_bottom = std::min(bar_bottom, y - 1 + kLine);
        const bool lit = hl_top < hl_bottom;
        const uint16_t bg = lit && hl_top == y - 1 && hl_bottom == y - 1 + kLine ? kSurface : kBg;
        // Rows beside the corner mascot: the background runs up to it (the mascot
        // paints the same colour behind itself, so a selected row shows through),
        // the text stops short of it.
        const int right = mascot_on && y - 1 + kLine > mascot::kTop ? mascot::kLeft - 2 : kWidth;
        const bool short_row = right < kWidth;
        const int fill_w = short_row ? mascot::kLeft : kWidth;
        g.fillRect(0, y - 1, fill_w, kLine, bg);  // each row clears itself
        if (lit && bg != kSurface) g.fillRect(0, hl_top, fill_w, hl_bottom - hl_top, kSurface);
        if (short_row && y - 1 < mascot::kTop) {  // the slice of the row above the mascot runs to the edge
            g.fillRect(mascot::kLeft, y - 1, mascot::kSize, mascot::kTop - (y - 1), bg);
            const int lit_end = std::min(hl_bottom, mascot::kTop);
            if (lit && bg != kSurface && hl_top < lit_end) g.fillRect(mascot::kLeft, hl_top, mascot::kSize, lit_end - hl_top, kSurface);
        }
        if (short_row) {
            mascot::set_backdrop(y - 1, kLine, bg);
            if (lit) mascot::set_backdrop(hl_top, hl_bottom - hl_top, kSurface);
            const int clip_top = std::max(y - 1, region_top);
            g.setClipRect(0, clip_top, right, std::max(0, std::min(y - 1 + kLine, region_bottom) - clip_top));
        }
        if (attention) g.fillRect(0, y - 1, 2, kLine, kWarn);
        else if (lit) g.fillRect(0, hl_top, 2, hl_bottom - hl_top, kAccent);
        const bool partly = lit && bg != kSurface;  // text over two colours: transparent
        const auto text_color = [&](uint16_t fg) {
            if (partly) g.setTextColor(fg);
            else g.setTextColor(fg, bg);
        };
        if (row.head) {
            text_color(kMuted);
            g.drawString(e.time, kPad, y);
            text_color(e.status == Status::Cancelled ? kMuted : sel ? kAccent : kText);
            g.drawString(short_row ? fit(g, row.text, right - kPad * 2 - 44 - 14) : row.text, kPad + 44, y);
            icon(g, right - kPad - 10, y + 1, e.status, sel);
        } else if (row.text == "\x01stage") {
            draw_stage(g, kPad + row.indent, y, e, bg);
        } else {
            text_color(row.color);
            g.drawString(short_row ? fit(g, row.text, right - kPad - row.indent) : row.text, kPad + row.indent, y);
        }
        if (short_row) g.setClipRect(0, region_top, kWidth, region_bottom - region_top);
    }
    g.clearClipRect();

    // Clear what the rows did not cover.
    const int rows_end = std::clamp(top - 1 + static_cast<int>(rows.size()) * kLine - scroll_px, region_top, region_bottom);
    if (mascot_on) {  // ...except the mascot's corner, which is drawn last
        g.fillRect(0, rows_end, mascot::kLeft, kHeight - rows_end, kBg);
        if (rows_end < mascot::kTop) g.fillRect(mascot::kLeft, rows_end, mascot::kSize, mascot::kTop - rows_end, kBg);
    } else {
        g.fillRect(0, rows_end, kWidth, kHeight - rows_end, kBg);
    }
    if (count == 0) {
        g.setTextColor(kMuted, kBg);
        g.drawCentreString("还没有对话", kWidth / 2, 54);
    }
    if (footer) {
        const int y = kHeight - kLine;
        const int span = mascot_on ? mascot::kLeft - 2 : kWidth;  // centred left of the mascot
        if (!model.notice.isEmpty()) {
            g.setTextColor(model.notice_error ? kDanger : kMuted, kBg);
            g.drawCentreString(fit(g, model.notice, span - 8), span / 2, y);
        } else if (!model.agent_draft.isEmpty()) {
            g.setTextColor(kText, kBg);
            // The newest segment is at the end: keep the end in view.
            const String label = "草稿：";
            g.drawString(label + fit_tail(g, model.agent_draft, span - 8 - 70 - g.textWidth(label)), kPad, y);
            g.setTextColor(kMuted, kBg);
            g.drawRightString("Enter 发送", span - 4, y);
        } else {
            g.setTextColor(kMuted, kBg);
            g.drawCentreString(voice_mode == VoiceMode::Agent ? "按住 Enter 说话" : "按住 Enter 说话（只转写）",
                               span / 2, y);
        }
    }
    if (mascot_on) mascot::push(g);
}

// ---------------------------------------------------------------- secret input

void draw_secret(Gfx& g, const Model& model) {
    const int top = 50;
    g.fillRect(0, top - 4, kWidth, kHeight - top + 4, kBg);
    g.drawFastHLine(0, top - 4, kWidth, kSurface);
    g.setFont(&cjk::cn12);
    g.setTextColor(kAccent, kBg);
    g.drawString("输入密码", kPad, top);
    g.setTextColor(kMuted, kBg);
    const String prompt = model.pending.empty() ? String() : model.pending.front().secret_prompt;
    g.drawString(fit(g, prompt, kWidth - kPad * 2), kPad, top + kLine);
    const int box = top + kLine * 2 + 2;
    g.fillRoundRect(kPad, box, kWidth - kPad * 2, kLine + 4, 3, kSurface);
    int x = kPad * 2;
    if (secret_reveal) {
        g.setTextColor(kText, kSurface);
        const String shown = fit(g, secret_buffer, kWidth - kPad * 5);
        g.drawString(shown, x, box + 2);
        x += g.textWidth(shown) + 2;
    } else {
        for (std::size_t i = 0; i < secret_buffer.length() && x < kWidth - kPad * 3; ++i) {
            if ((static_cast<unsigned char>(secret_buffer[i]) & 0xC0) == 0x80) continue;
            g.fillCircle(x + 3, box + 8, 3, kText);
            x += 9;
        }
    }
    if ((millis() / 500) % 2) g.fillRect(x + 1, box + 3, 2, kLine - 2, kAccent);
    g.setTextColor(kMuted, kBg);
    g.drawString("Enter 确认  ` 取消  Fn+S 显示", kPad, box + kLine + 8);
    g.drawString("仅本机使用，不发给语音或 AI", kPad, box + kLine * 2 + 8);
}

// ---------------------------------------------------------------- detail

void build_detail(const Model& model, int index) {
    detail_lines.clear();
    detail_scroll = 0;
    if (index < 0 || index >= static_cast<int>(model.timeline.size())) return;
    const Entry& e = model.timeline[index];
    detail_id = e.id;
    auto& g = M5Cardputer.Display;
    g.setFont(&cjk::cn12);
    const int w = kWidth - kPad * 2;
    auto add = [&](const String& text, uint16_t color, int max_lines) {
        for (const auto& line : wrap(g, text, w, max_lines)) detail_lines.push_back({line, color});
    };
    add(e.text, kMuted, 3);
    detail_lines.push_back({"", kBg});
    // RAM keeps a 300-byte preview; a longer reply is in its own file.
    String reply = entry_reply(e.id);
    if (reply.isEmpty()) reply = e.summary;
    add(reply.isEmpty() ? String("（进行中）") : reply, e.status == Status::Failed ? kDanger : kText, 60);
    cJSON* record = cJSON_Parse(entry_detail(e.id).c_str());
    const cJSON* step = nullptr;
    cJSON* steps = record ? cJSON_GetObjectItemCaseSensitive(record, "steps") : nullptr;
    cJSON_ArrayForEach(step, steps) {
        detail_lines.push_back({"", kBg});
        const String decision = json_str(step, "decision");
        const String mark = decision == "approved" || decision == "approved_all" ? "已批准 · "
                            : decision == "rejected" ? "已拒绝 · "
                            : decision == "deny" ? "已拦截 · " : "";
        add(mark + json_str(step, "what"), decision == "rejected" || decision == "deny" ? kWarn : kAccent, 2);
        add(json_str(step, "result"), kMuted, 12);
    }
    cJSON_Delete(record);
}

void draw_detail(Gfx& g, const Model& model) {
    g.setFont(&cjk::cn12);
    const int top = kBarHeight + 2;
    const Entry* entry = nullptr;
    for (const auto& e : model.timeline) {
        if (e.id == detail_id) entry = &e;
    }
    g.setTextColor(kMuted, kBg);
    String head = "< " + (entry ? entry->time : String()) + "   , 返回";
    g.drawString(head, kPad, top);
    if (entry) {
        const String took = String(entry->elapsed_ms / 1000.0f, 1) + "s";
        g.drawString(took, kWidth - kPad - g.textWidth(took), top);
        icon(g, kWidth - kPad - g.textWidth(took) - 14, top + 1, entry->status, false);
    }
    const int visible = (kHeight - top - kLine) / kLine;
    detail_scroll = std::max(0, std::min(detail_scroll, static_cast<int>(detail_lines.size()) - visible));
    for (int i = 0; i < visible && detail_scroll + i < static_cast<int>(detail_lines.size()); ++i) {
        const auto& line = detail_lines[detail_scroll + i];
        g.setTextColor(line.second, kBg);
        g.drawString(line.first, kPad, top + kLine * (i + 1));
    }
}

// ---------------------------------------------------------------- help

void draw_help(Gfx& g) {
    g.setFont(&cjk::cn12);
    const int top = kBarHeight + 2;
    const char* titles[] = {"按键", "设置（直接说）", "能做的事"};
    g.setTextColor(kAccent, kBg);
    g.drawString(String("帮助 · ") + titles[help_page], kPad, top);
    g.setTextColor(kMuted, kBg);
    const String pager = String(help_page + 1) + "/3  , /";
    g.drawString(pager, kWidth - kPad - g.textWidth(pager), top);
    int y = top + kLine + 2;
    auto row = [&](const String& left, const String& right, uint16_t right_color) {
        g.setTextColor(kText, kBg);
        g.drawString(fit(g, left, 70), kPad, y);
        g.setTextColor(right_color, kBg);
        g.drawString(fit(g, right, kWidth - kPad * 2 - 74), kPad + 74, y);
        y += kLine;
    };
    if (help_page == 0) {
        row("Enter", "按住说话   Fn+Enter 说进草稿", kMuted);
        row("Btn0", "切换页面：Agent SSH 程序 图形", kMuted);
        row("; .", "上下选择   / 详情   , 返回", kMuted);
        row("Y N A", "执行 / 取消 / 后续都执行", kMuted);
        row("`", "取消当前任务", kMuted);
        row("Fn+A", "程序页   Fn+W 设置", kMuted);
        row("Fn+Tab", "只转写   Fn+Space 息屏", kMuted);
        return;
    }
    std::vector<registry::Package::Hint> rows;
    for (const auto& hint : registry::hints()) {
        if ((help_page == 1) == !hint.value.isEmpty()) rows.push_back(hint);
    }
    if (rows.empty()) return;
    help_cursor = std::clamp(help_cursor, 0, static_cast<int>(rows.size()) - 1);
    if (help_detail) {  // one row in full: what it is, how to say it, the package's usage
        const auto& hint = rows[help_cursor];
        String text = hint.name + (hint.value.isEmpty() ? String() : "：当前 " + hint.value) + "\n怎么说：“" +
                      hint.example + "”\n\n" + registry::package_help(hint.package);
        const auto lines = wrap_text(g, text, kWidth - kPad * 2);
        const int visible = (kHeight - y - kLine) / kLine;
        help_scroll = std::clamp(help_scroll, 0, std::max(0, static_cast<int>(lines.size()) - visible));
        for (int i = help_scroll; i < static_cast<int>(lines.size()) && i < help_scroll + visible; ++i, y += kLine) {
            g.setTextColor(i < 2 ? kText : kMuted, kBg);
            g.drawString(lines[i], kPad, y);
        }
        g.setTextColor(kMuted, kBg);
        g.drawRightString("; . 滚动  , 返回", kWidth - kPad, kHeight - kLine);
        return;
    }
    const int visible = (kHeight - y - kLine) / kLine;
    const int count = rows.size();
    const int first = std::clamp(help_cursor - visible + 1, 0, std::max(0, count - visible));
    const int bar_at = help_bar.follow(help_cursor * kLine, help_page);
    const int scroll_px = help_list_scroll.follow(first * kLine, help_page);
    scrolled_rows(g, y, std::min(visible, count), scroll_px, count, bar_at, [&](Gfx& b, int i, int ty) {
        const auto& hint = rows[i];
        b.setTextColor(i == help_cursor ? kAccent : kText);
        b.drawString(fit(b, hint.name, 52), kPad, ty);
        int x = kPad + 56;
        if (!hint.value.isEmpty()) {
            b.setTextColor(kAccent);
            const String value = fit(b, hint.value, 60);
            b.drawString(value, x, ty);
            x += std::max<int>(64, b.textWidth(value) + 6);
        }
        b.setTextColor(kMuted);
        b.drawString(fit(b, "“" + hint.example + "”", kWidth - x - kPad), x, ty);
    });
    g.setTextColor(kMuted, kBg);
    g.drawRightString("; . 选择  Enter 详情", kWidth - kPad, kHeight - kLine);
}

// ---------------------------------------------------------------- SSH page

void draw_terminal(Gfx& g, const Model& model) {
    const TermFont tf = term_font();
    g.setFont(tf.font);
    const int top = kBarHeight + 2;
    String body = model.terminal_text;
    while (body.endsWith("\n") || body.endsWith(" \n")) body = body.substring(0, body.length() - 1);
    if (model.terminal_password) {
        String masked;
        for (std::size_t i = 0; i < model.terminal_draft.length(); ++i) {
            if ((static_cast<unsigned char>(model.terminal_draft[i]) & 0xC0) != 0x80) masked += "*";
        }
        body += "\n" + masked + "_";
    } else if (!model.terminal_draft.isEmpty()) {
        // After the remote prompt, where it will appear once sent; capture-pane
        // drops the space after a prompt sign, so put it back for display.
        const char last = body.length() ? body[body.length() - 1] : 0;
        if (last == 0x24 || last == 0x23 || last == 0x3E || last == 0x25 || last == 0x3A) body += " ";
        body += model.terminal_draft + "_";
    }
    const int bottom = model.terminal_password ? kHeight - tf.line - 2 : kHeight;
    const int visible = (bottom - top) / tf.line;
    auto lines = wrap(g, body, kWidth - 4, 200);
    const int first = std::max(0, static_cast<int>(lines.size()) - visible);
    const bool offline = model.terminal_title == "未连接";
    // Each row is cleared just before it is drawn, so the screen never goes blank.
    for (int row = 0; row < visible; ++row) {
        const int y = top + row * tf.line;
        g.fillRect(0, y, kWidth, tf.line, kBg);
        const int i = first + row;
        if (i >= static_cast<int>(lines.size())) continue;
        g.setTextColor(offline ? kMuted : kText, kBg);
        g.drawString(lines[i], 2, y);
    }
    const int used = top + visible * tf.line;
    if (used < bottom) g.fillRect(0, used, kWidth, bottom - used, kBg);
    if (top > kBarHeight) g.fillRect(0, kBarHeight, kWidth, top - kBarHeight, kBg);
    if (model.terminal_password) {
        g.fillRect(0, kHeight - tf.line - 1, kWidth, tf.line + 1, kSurface);
        g.setTextColor(kWarn, kSurface);
        g.drawString("远端在等密码：输入后 Enter（不显示、不记录）", 2, kHeight - tf.line);
    }
}

// ---------------------------------------------------------------- render

void note_activity() { last_activity = millis(); }

void wake_screen() {
    if (!screen_on) {
        M5Cardputer.Display.setBrightness(config::get().brightness);
        screen_on = true;
        drawn_version = UINT32_MAX;
    }
    note_activity();
}

void sleep_screen() {
    M5Cardputer.Display.setBrightness(0);
    screen_on = false;
}

bool secret_active(const Model& model) {
    return current_page == Page::Agent && !model.busy && !model.pending.empty() &&
           !model.pending.front().secret_prompt.isEmpty();
}

// ---------------------------------------------------------------- the program page

void switch_page(Page next);

void refresh_program_items() {
    program_items.clear();
    delete_pending = -1;
    try {  // the list is rebuilt while a task may hold most of the heap: a failed allocation shows a shorter list
        for (const auto& p : programs::runnable()) program_items.push_back({p.name, p.about, false, p.used});
    } catch (const std::bad_alloc&) {
        runtime_event("low_memory", "program list");
    }
    cJSON* list = cJSON_Parse(storage::list_dir("/vat/things/apps").c_str());
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        const String file = json_str(entry, "name");
        if (!file.endsWith(".json")) continue;
        const cJSON* mtime = cJSON_GetObjectItemCaseSensitive(entry, "mtime");
        program_items.push_back({file.substring(0, file.length() - 5), String(), true,
                                 cJSON_IsNumber(mtime) ? static_cast<time_t>(mtime->valuedouble) : 0});
    }
    cJSON_Delete(list);
    std::stable_sort(program_items.begin(), program_items.end(),
                     [](const ProgramItem& a, const ProgramItem& b) { return a.used > b.used; });
    program_items_loaded = true;
    program_items_stale = false;
    program_items_at = millis();
    program_items_gen = programs::generation();
    program_items_run = console::head().started;
    program_cursor = std::clamp(program_cursor, 0, std::max(0, static_cast<int>(program_items.size()) - 1));
}

constexpr int kProgramTop = kBarHeight + 3;  // the first text row
constexpr int kProgramRows = (kHeight - kLine - kProgramTop + 1) / kLine;

void program_footer(Gfx& screen, const String& text, uint16_t color) {
    band(screen, kHeight - kLine, kLine, kBg, [&](Gfx& g, int top) {
        g.setTextColor(color, kBg);
        g.drawString(fit_tail(g, text, kWidth - kPad * 2), kPad, top);
    });
}

void draw_program_list(Gfx& g) {
    const int count = program_items.size();
    if (!program_items_loaded) {
        g.fillRect(0, kBarHeight, kWidth, kHeight - kBarHeight, kBg);
        g.setTextColor(kMuted, kBg);
        g.drawCentreString("读取中…", kWidth / 2, 54);
        return;
    }
    if (count == 0) {
        g.fillRect(0, kBarHeight, kWidth, kHeight - kBarHeight, kBg);
        g.setTextColor(kMuted, kBg);
        g.drawCentreString("还没有程序或本地应用", kWidth / 2, 46);
        g.drawCentreString("对大肥鱼说：写个程序……", kWidth / 2, 62);
        return;
    }
    const int first = std::max(0, std::min(program_cursor - kProgramRows / 2, count - kProgramRows));
    const int bar_at = program_bar.follow(program_cursor * kLine, 0);
    const int scroll_px = program_scroll.follow(first * kLine, 0);
    const int slots = std::min(kProgramRows, count);
    scrolled_rows(g, kProgramTop, slots, scroll_px, count, bar_at, [&](Gfx& b, int i, int y) {
        const auto& item = program_items[i];
        const String kind = item.app ? "应用" : "程序";
        b.setTextColor(kMuted);
        b.drawRightString(kind, kWidth - kPad, y);
        b.setTextColor(i == program_cursor ? kAccent : kText);
        String left = item.name;
        if (!item.about.isEmpty()) left += "  " + item.about;
        b.drawString(fit(b, left, kWidth - kPad * 3 - b.textWidth(kind)), kPad, y);
    });
    const int below = kProgramTop - 1 + slots * kLine;
    if (below < kHeight - kLine) g.fillRect(0, below, kWidth, kHeight - kLine - below, kBg);
    if (delete_pending >= 0 && delete_pending < count) {
        program_footer(g, "删除「" + program_items[delete_pending].name + "」？Y 删除  N 取消", kWarn);
    } else {
        program_footer(g, console::head().name.isEmpty() ? "; . 选择  Enter 运行  Del 删除"
                                                         : "; . 选择  Enter 运行  Del 删除  / 上次输出", kMuted);
    }
}

void draw_console(Gfx& g) {
    const console::View v = console::view();
    std::vector<std::pair<String, bool>> rows;  // display rows: text, error
    g.setFont(&cjk::cn12);
    for (const auto& line : v.lines) {
        if (line.text.isEmpty()) {
            rows.push_back({String(), line.error});
            continue;
        }
        for (const auto& part : wrap_text(g, line.text, kWidth - kPad * 2)) rows.push_back({part, line.error});
    }
    const int total = rows.size();
    output_scroll = std::clamp(output_scroll, 0, std::max(0, total - kProgramRows));
    const int first = std::max(0, total - kProgramRows - output_scroll);
    for (int k = 0; k < kProgramRows; ++k) {
        const int i = first + k;
        band(g, kProgramTop + k * kLine - 1, kLine, kBg, [&](Gfx& b, int top) {
            if (i >= total) return;
            b.setTextColor(rows[i].second ? kDanger : kText, kBg);
            b.drawString(rows[i].first, kPad, top + 1);
        });
    }
    if (v.wait == console::Wait::Line) {
        program_footer(g, "> " + v.prompt + v.input + "_", kAccent);
    } else if (v.wait == console::Wait::Key) {
        program_footer(g, "按键继续   ` 中断", kAccent);
    } else if (v.running) {
        program_footer(g, "运行中   ` 中断", kMuted);
    } else if (!v.name.isEmpty()) {
        program_footer(g, v.status + (output_scroll ? "   ; . 滚动" : "") + "   Enter 返回列表", v.failed ? kDanger : kMuted);
    } else {
        program_footer(g, "Enter 返回列表", kMuted);
    }
}

// Redrawn on its own changes: the console's, the list's, the seconds while running.
void draw_program_page(bool force, uint32_t version_now) {
    const uint32_t console_now = console::version();
    const console::Head h = console::head();
    const uint32_t second = h.running ? millis() / 1000 : 0;
    const bool sliding = program_bar.sliding() || program_scroll.sliding();
    if (!force && version_now == drawn_version && console_now == console_drawn && second == console_second && !sliding) return;
    drawn_version = version_now;
    console_drawn = console_now;
    console_second = second;
    Gfx& g = M5Cardputer.Display;
    g.startWrite();
    g.setTextWrap(false, false);
    // Coming from another page (the Agent page leaves its stage line at the
    // bottom) or between list and console: start from a clear screen once.
    const int layout = program_list ? 1 : 2;
    if (layout != program_layout) {
        g.fillScreen(kBg);
        program_layout = layout;
    }
    top_bar(g, snapshot());
    // The strips the rows and the footer do not cover.
    g.fillRect(0, kBarHeight, kWidth, kProgramTop - 1 - kBarHeight, kBg);
    const int rows_end = kProgramTop - 1 + kProgramRows * kLine;
    g.fillRect(0, rows_end, kWidth, kHeight - kLine - rows_end, kBg);
    if (program_list) draw_program_list(g);
    else draw_console(g);
    g.endWrite();
}

String key_name(const Keyboard_Class::KeysState& state) {
    String name;
    if (state.enter) name = "enter";
    else if (state.del) name = "del";
    else if (state.tab) name = "tab";
    else if (!state.word.empty()) name = state.word[0] == ' ' ? String("space") : String(state.word[0]);
    else return {};
    return state.fn ? "fn+" + name : name;
}

void open_program_item(const ProgramItem& item) {
    if (!item.app) {
        if (!programs::launch(item.name)) set_notice("正在处理上一个任务，` 可取消");
        return;
    }
    cJSON* call = cJSON_CreateObject();  // the ui package opens it on the graphics page
    cJSON_AddStringToObject(call, "tool", "write");
    cJSON* arguments = cJSON_AddObjectToObject(call, "arguments");
    cJSON_AddStringToObject(arguments, "target", ("ui://apps/" + item.name + "/open").c_str());
    cJSON_AddStringToObject(arguments, "content", "");
    cJSON_AddStringToObject(arguments, "mode", "overwrite");
    cJSON_AddBoolToObject(arguments, "confirm_required", false);
    worker::Job job;
    job.kind = worker::Kind::Call;
    job.text = json_print(call);
    cJSON_Delete(call);
    if (worker::submit(job) == 0) set_notice("正在处理上一个任务，` 可取消");
}

void program_key(const Keyboard_Class::KeysState& state, bool esc) {
    const console::Wait wait = console::waiting();
    if (wait == console::Wait::Key) {
        const String name = key_name(state);
        if (!name.isEmpty()) console::key(name);
        return;
    }
    if (wait == console::Wait::Line) {
        if (state.enter) console::submit();
        else if (state.del) console::erase();
        else {
            String typed;
            for (const char c : state.word) typed += c;
            if (!typed.isEmpty()) console::type(typed);
        }
        return;
    }
    if (console::head().running) return;  // ` interrupts it (handled before)
    if (!program_list) {
        if (has_char(state, ';')) ++output_scroll;
        else if (has_char(state, '.')) output_scroll = std::max(0, output_scroll - 1);
        else if (state.enter || esc || has_char(state, ',')) {
            program_list = true;
            program_items_stale = true;  // a run may have changed the order
        }
        drawn_version = UINT32_MAX;
        return;
    }
    const int count = program_items.size();
    if (delete_pending >= 0) {  // Del asked: Y deletes, anything else cancels
        if (has_char(state, 'y') && delete_pending < count) {
            const ProgramItem item = program_items[delete_pending];
            String result;
            if (item.app) {
                result = storage::remove("/vat/things/apps/" + item.name + ".json") ? "已删除应用「" + item.name + "」"
                                                                                     : "ERROR: 删除失败";
            } else {
                result = programs::remove(item.name);
            }
            set_notice(result.startsWith("ERROR: ") ? result.substring(7) : result, result.startsWith("ERROR"));
            refresh_program_items();
        }
        delete_pending = -1;
        drawn_version = UINT32_MAX;
        return;
    }
    if (count && state.del) {
        delete_pending = program_cursor;
        drawn_version = UINT32_MAX;
        return;
    }
    if (count && has_char(state, ';')) program_cursor = std::max(0, program_cursor - 1);
    else if (count && has_char(state, '.')) program_cursor = std::min(count - 1, program_cursor + 1);
    else if (has_char(state, '/') && !console::head().name.isEmpty()) {
        program_list = false;
        output_scroll = 0;
    } else if (state.enter && count) {
        open_program_item(program_items[program_cursor]);
    }
    drawn_version = UINT32_MAX;
}

// What the corner mascot shows for the newest entry.
mascot::State mascot_state(const Model& model) {
    if (model.timeline.empty()) return mascot::State::Idle;
    const Entry& e = model.timeline.back();
    if (e.status == Status::Failed) return mascot::State::Error;
    if (e.status != Status::Running) return mascot::State::Idle;
    if (e.stage.startsWith("STT")) return mascot::State::Recognize;
    if (e.stage.startsWith("RUN") || e.stage.startsWith("STEP")) return mascot::State::Execute;
    return mascot::State::Understand;
}

void render(bool force = false) {
    if (!screen_on) return;
    if (settings::active()) {
        settings::draw(force);
        return;
    }
    if (current_page == Page::Graphics) {
        if (overlay::open()) {
            overlay::draw(force);
            return;
        }
        switch_page(previous_page);  // its picture closed meanwhile
        force = true;
    }
    if (current_page == Page::Program && view == View::List) {
        draw_program_page(force, version());
        return;
    }
    // The highlight or the rows move: redraw them each frame.
    if (timeline_sliding() || help_bar.sliding() || help_list_scroll.sliding()) force = true;
    const uint32_t current = version();
    // Mascot frames: a 32x32 push of a frame read beforehand, nothing else redrawn.
    if (!force && current == drawn_version && mascot_live && mascot::due() && mascot::load()) {
        Gfx& g = M5Cardputer.Display;
        g.startWrite();
        mascot::push(g);
        g.endWrite();
    }
    // A status item changed on its own (the minute, the battery): the bar only.
    if (!force && current == drawn_version && status::changed()) {
        Gfx& g = M5Cardputer.Display;
        g.startWrite();
        top_bar(g, snapshot());
        g.endWrite();
    }
    const bool animating = (is_busy() || has_pending()) && millis() - last_draw > 400;
    if (!force && current == drawn_version && !animating) return;
    Gfx& g = M5Cardputer.Display;
    last_draw = millis();
    if (!force && current == drawn_version) {  // animation frame: only the stage dot changes
        if (stage_dot.valid && current_page == Page::Agent && view == View::List) {
            g.startWrite();
            g.fillCircle(stage_dot.x, stage_dot.y, 1, (millis() / 400) % 2 ? kAccent : stage_dot.bg);
            g.endWrite();
        }
        return;
    }
    const Model model = snapshot();
    drawn_version = current;
    stage_dot.valid = false;
    // Leaving help for the terminal page: the terminal only overwrites its rows.
    static bool shown_help = false;
    const bool last_drawn_help = shown_help;
    shown_help = view == View::Help;
    const bool timeline = view != View::Help && current_page != Page::Terminal && view != View::Detail;
    const bool secret = timeline && secret_active(model);
    mascot_live = timeline && !secret && mascot::enabled();
    if (mascot_live) {  // the card is read before the display transaction starts
        mascot::request(mascot_state(model));
        mascot_live = mascot::load();
    }
    // No full-screen sprite (64 KB): drawn directly, the timeline row by row
    // without blanking first.
    // Pages that are not drawn row by row are cleared when their layout
    // changes, not on every redraw (help flashed at every key).
    static int drawn_layout = -1;
    int layout = static_cast<int>(current_page) * 16 + static_cast<int>(view) * 4;
    if (view == View::Help) layout = layout * 256 + help_page * 2 + (help_detail ? 1 : 0) + (help_detail ? help_scroll * 8 : 0);
    if (view == View::Detail) layout = layout * 4096 + detail_scroll;
    const bool clear = !timeline && current_page != Page::Terminal && layout != drawn_layout;
    drawn_layout = layout;
    const auto paint = [&](Gfx& t) {
        if (clear) t.fillScreen(kBg);  // the terminal clears row by row
        t.setTextWrap(false, false);
        top_bar(t, model);
        if (view == View::Help) {
            draw_help(t);
        } else if (current_page == Page::Terminal) {
            if (last_drawn_help) t.fillScreen(kBg);
            draw_terminal(t, model);
        } else if (view == View::Detail) {
            draw_detail(t, model);
        } else {
            draw_timeline(t, model, mascot_live);
            if (secret) draw_secret(t, model);
        }
    };
    g.startWrite();
    paint(g);
    g.endWrite();
}

// Page side of a recording: the screen is on and current before the meter
// strip is drawn over it, and fully redrawn afterwards.
bool record(bool (*held)()) {
    wake_screen();
    render(true);
    if (mascot_live) {  // the meter strip covers only part of the corner: hide it while recording
        M5Cardputer.Display.fillRect(mascot::kLeft, mascot::kTop, mascot::kSize, mascot::kSize, kBg);
        mascot_live = false;
    }
    const bool ok = recorder::record(held);
    drawn_version = UINT32_MAX;
    return ok;
}

void dismiss_overlay() {
    overlay::dismiss();
    if (current_page == Page::Graphics) switch_page(previous_page);
    drawn_version = UINT32_MAX;  // redraw the page underneath
}

bool enter_held() { return M5Cardputer.Keyboard.keysState().enter; }

unsigned long simulated_release_ms = 0;
bool simulated_hold() { return millis() < simulated_release_ms; }

// ---------------------------------------------------------------- input

void submit_or_busy(const worker::Job& job) {
    if (worker::submit(job) == 0) set_notice("正在处理上一个任务，` 可取消");
}

void switch_page(Page next) {
    if (next == Page::Graphics && !overlay::open()) return;
    if (current_page == Page::Graphics && next != Page::Graphics) overlay::suspend();
    if (next == Page::Graphics && current_page != Page::Graphics) previous_page = current_page;
    if (next == Page::Program && current_page != Page::Program) {
        // Drawn now from the cache; loop() refreshes it a frame later if needed.
        if (!program_items_loaded || programs::generation() != program_items_gen ||
            console::head().started != program_items_run || millis() - program_items_at > 60000) {
            program_items_stale = true;
        }
        program_layout = -1;  // drawn from a clear screen
    }
    if (next != current_page) timeline_scroll.reset();
    current_page = next;
    view = View::List;
    drawn_version = UINT32_MAX;
    if (next == Page::Terminal && !worker::busy() && draft_empty()) {
        worker::submit({worker::Kind::TerminalObserve});
        last_terminal_refresh = millis();
    }
}

// Btn0: Agent, SSH, program, graphics (only while a picture is open), Agent…
Page next_page() {
    switch (current_page) {
        case Page::Agent: return Page::Terminal;
        case Page::Terminal: return Page::Program;
        case Page::Program: return overlay::open() ? Page::Graphics : Page::Agent;
        default: return Page::Agent;
    }
}

void send_terminal(const String& text, std::vector<String> keys, bool submit) {
    worker::Job job;
    job.kind = worker::Kind::TerminalInput;
    job.text = text;
    job.keys = std::move(keys);
    job.submit = submit;
    submit_or_busy(job);
}

void move_cursor(int delta) {
    const int count = snapshot().timeline.size();
    if (count == 0) return;
    int index = cursor < 0 ? count - 1 : cursor;
    index = std::clamp(index + delta, 0, count - 1);
    cursor = index;
    drawn_version = UINT32_MAX;
}

void open_detail() {
    const Model model = snapshot();
    const int count = model.timeline.size();
    if (count == 0) return;
    build_detail(model, cursor < 0 || cursor >= count ? count - 1 : cursor);
    view = View::Detail;
    drawn_version = UINT32_MAX;
}

void close_overlay() {
    view = View::List;
    drawn_version = UINT32_MAX;
}

void submit_secret(bool cancel_input) {
    worker::Job job;
    job.kind = worker::Kind::Secret;
    job.text = cancel_input ? String() : secret_buffer;
    wipe(secret_buffer);  // the copy lives only inside the job
    secret_reveal = false;
    submit_or_busy(job);
    wipe(job.text);
}

void dispatch_keys(const Keyboard_Class::KeysState& state);

// A held ; or . repeats (lists, the timeline, help, the console's output)
// after 400 ms, every 70 ms.  Not on the SSH page, where they go to the
// remote terminal, nor while a program reads single keys.
bool repeats(const Keyboard_Class::KeysState& state) {
    if (state.fn || state.ctrl || state.opt || state.enter || state.del || state.word.size() != 1) return false;
    if (state.word[0] != ';' && state.word[0] != '.') return false;
    if (current_page == Page::Terminal && !settings::active()) return false;
    return console::waiting() != console::Wait::Key;
}

void handle_keys() {
    static uint32_t held_since = 0;
    static uint32_t last_repeat = 0;
    auto& keyboard = M5Cardputer.Keyboard;
    if (keyboard.isChange()) {
        held_since = 0;
        if (!keyboard.isPressed()) return;
        const auto state = keyboard.keysState();
        dispatch_keys(state);
        if (repeats(state)) held_since = last_repeat = millis();
        return;
    }
    if (!held_since || !keyboard.isPressed()) return;
    const uint32_t now = millis();
    if (now - held_since < 400 || now - last_repeat < 70) return;
    const auto state = keyboard.keysState();
    if (!repeats(state)) {
        held_since = 0;
        return;
    }
    last_repeat = now;
    dispatch_keys(state);
}

void dispatch_keys(const Keyboard_Class::KeysState& state) {
    if (!screen_on) {
        wake_screen();
        render(true);
        if (!state.enter) return;
    }
    note_activity();
    if (current_page == Page::Graphics && overlay::open() && !settings::active()) {
        if (console::waiting() == console::Wait::Key) {  // a running program reads its keys (vat.key)
            const String name = key_name(state);
            if (!name.isEmpty()) console::key(name);
            return;
        }
        if (console::active() && has_char(state, '`')) {
            action("cancel");
            return;
        }
        if (overlay::is_panel()) {
            if (overlay::panel_key(state)) dismiss_overlay();
            else overlay::draw(false);
            return;
        }
        if (console::active()) return;  // a running program's picture stays until it ends
        dismiss_overlay();
        return;
    }
    if (settings::active()) {
        settings::handle(state);
        if (!settings::active()) drawn_version = UINT32_MAX;  // closed: redraw the page
        return;
    }
    const Model model = snapshot();
    const bool busy = worker::busy();
    const bool esc = has_char(state, '`');

    // Masked secret input takes every key while it is open.
    if (secret_active(model) && view == View::List) {
        if (state.fn && has_char(state, 's')) secret_reveal = !secret_reveal;
        else if (esc) submit_secret(true);
        else if (state.enter) submit_secret(false);
        else if (state.del) erase_last(secret_buffer);
        else for (const char c : state.word) secret_buffer += c;
        drawn_version = UINT32_MAX;
        return;
    }
    if (esc && view == View::Help && help_detail) {  // back to the list first
        help_detail = false;
        drawn_version = UINT32_MAX;
        return;
    }
    if (esc && (view == View::Help || view == View::Detail)) {
        close_overlay();
        return;
    }
    if (esc && (busy || agent::paused())) {
        action("cancel");
        return;
    }
    if (state.fn) {
        if (state.space) sleep_screen();
        else if (has_char(state, 'h')) { view = view == View::Help ? View::List : View::Help; help_page = 0; help_cursor = 0; help_detail = false; }
        else if (state.tab && current_page == Page::Agent) voice_mode = voice_mode == VoiceMode::Agent ? VoiceMode::Text : VoiceMode::Agent;
        else if ((has_char(state, '=') || has_char(state, '-')) && current_page == Page::Terminal) {
            static const uint8_t sizes[] = {10, 12, 14};
            const uint8_t now = config::get().terminal_font;
            int index = now == 10 ? 0 : now == 14 ? 2 : 1;
            index = constrain(index + (has_char(state, '=') ? 1 : -1), 0, 2);
            config::get().terminal_font = sizes[index];
            config::save_ui_number("terminal_font", sizes[index]);
            apply_terminal_size();
            render(true);
            if (!busy) worker::submit({worker::Kind::TerminalObserve});  // the remote window follows
        }
        else if (has_char(state, 'w')) enter_setup("设置");
        else if (has_char(state, 'a')) {  // the program page, at its list unless a program is running
            switch_page(Page::Program);
            if (!console::head().running && !program_list) {
                program_list = true;
                program_items_stale = true;
            }
        }
        else if (state.enter && current_page == Page::Agent && !busy && model.pending.empty()) {
            // A segment into the draft; Enter alone sends the draft.
            if (record(enter_held)) submit_or_busy({worker::Kind::DraftPcm, "agent", "/vat/cache/ptt.pcm"});
        }
        else if (has_char(state, 'l') && current_page == Page::Agent) open_detail();
        else if (has_char(state, 'j') && current_page == Page::Agent) close_overlay();
        else if (has_char(state, 'v') && current_page == Page::Terminal) set_draft(model.terminal_draft + local::clipboard());
        else if (has_char(state, 'r') && current_page == Page::Terminal && !busy) worker::submit({worker::Kind::TerminalObserve});
        else if (has_char(state, ';') && current_page == Page::Terminal) send_terminal("", {"up"}, false);
        else if (has_char(state, '.') && current_page == Page::Terminal) send_terminal("", {"down"}, false);
        else if (state.del && current_page == Page::Agent) { agent::clear_history(); set_notice("对话记忆已清空"); }
        else if (state.enter && current_page == Page::Terminal && !busy) {
            if (record(enter_held)) submit_or_busy({worker::Kind::DraftPcm, "", "/vat/cache/ptt.pcm"});
        }
        drawn_version = UINT32_MAX;
        return;
    }
    if (state.opt && (has_char(state, ',') || has_char(state, '.'))) {
        auto& cfg = config::get();
        std::size_t index = 0;
        for (std::size_t i = 0; i < std::size(kScreenTimeouts); ++i) {
            if (kScreenTimeouts[i] == cfg.screen_timeout_seconds) index = i;
        }
        index = has_char(state, '.') ? (index + 1) % std::size(kScreenTimeouts)
                                     : (index + std::size(kScreenTimeouts) - 1) % std::size(kScreenTimeouts);
        cfg.screen_timeout_seconds = kScreenTimeouts[index];
        config::save_ui_number("screen_timeout_seconds", cfg.screen_timeout_seconds);
        set_notice("息屏时间：" + (cfg.screen_timeout_seconds ? String(cfg.screen_timeout_seconds) + " 秒" : String("永不")));
        return;
    }
    if (view == View::Help) {
        if (help_detail) {
            if (has_char(state, ';')) help_scroll -= 3;
            else if (has_char(state, '.')) help_scroll += 3;
            else if (has_char(state, ',')) help_detail = false;
        } else {
            const int before = help_page;
            if (has_char(state, ',')) help_page = std::max(0, help_page - 1);
            if (has_char(state, '/')) help_page = std::min(2, help_page + 1);
            if (help_page != before) help_cursor = 0;
            if (help_page > 0 && has_char(state, ';')) --help_cursor;
            if (help_page > 0 && has_char(state, '.')) ++help_cursor;
            if (help_page > 0 && state.enter) {
                help_detail = true;
                help_scroll = 0;
            }
            help_cursor = std::max(0, help_cursor);
        }
        drawn_version = UINT32_MAX;
        return;
    }
    if (current_page == Page::Program) {
        program_key(state, esc);
        return;
    }
    if (current_page == Page::Agent) {
        if (view == View::Detail) {
            if (has_char(state, ';')) detail_scroll -= 3;
            else if (has_char(state, '.')) detail_scroll += 3;
            else if (has_char(state, ',')) close_overlay();
            drawn_version = UINT32_MAX;
            return;
        }
        if (!model.pending.empty() && !busy) {
            if (has_char(state, 'y')) { action("approve"); return; }
            if (has_char(state, 'n')) { action("reject"); return; }
            if (has_char(state, 'a')) { action("approve_all"); return; }
        }
        if (!model.agent_draft.isEmpty() && !busy && model.pending.empty()) {
            if (state.enter) {
                cursor = -1;
                submit_or_busy({worker::Kind::AgentText, take_agent_draft()});
                return;
            }
            if (esc) { take_agent_draft(); return; }
            if (state.del) {
                String draft = model.agent_draft;
                erase_last(draft);
                set_agent_draft(draft);
                return;
            }
        }
        if (has_char(state, ';')) { move_cursor(-1); return; }
        if (has_char(state, '.')) { move_cursor(1); return; }
        if (has_char(state, '/')) { open_detail(); return; }
        if (has_char(state, ',')) { cursor = -1; drawn_version = UINT32_MAX; return; }
        if (state.enter && !busy && model.pending.empty()) {
            cursor = -1;
            const bool text_mode = voice_mode == VoiceMode::Text;
            if (record(enter_held)) {
                submit_or_busy({text_mode ? worker::Kind::TranscribePcm : worker::Kind::AgentPcm, "",
                                "/vat/cache/ptt.pcm"});
            }
        }
        return;
    }
    // SSH page: the keyboard types into the remote terminal.  The draft is
    // local, so typing never waits for the 5 s screen refresh; sending queues
    // behind it (the worker queue holds two jobs).
    if (state.ctrl && has_char(state, 'c')) { send_terminal("", {"ctrl_c"}, false); return; }
    if (esc) { send_terminal("", {"esc"}, false); return; }
    if (state.tab) { send_terminal("", {"tab"}, false); return; }
    String draft = model.terminal_draft;
    if (state.enter) {
        worker::Job job;
        job.kind = worker::Kind::TerminalInput;
        job.text = draft;  // a password draft is never displayed or logged
        job.submit = true;
        if (worker::submit(job)) take_draft();
        else set_notice("终端忙，稍后再按 Enter（草稿还在）");
        return;
    }
    if (state.del) {
        if (draft.isEmpty()) send_terminal("", {"backspace"}, false);
        else {
            erase_last(draft);
            set_draft(draft);
        }
        return;
    }
    for (const char c : state.word) draft += c;
    if (!state.word.empty()) set_draft(draft);
}

void apply_hardware_requests() {
    int brightness;
    if (take_brightness(brightness)) {
        M5Cardputer.Display.setBrightness(brightness);
        screen_on = true;
    }
    bool on;
    if (take_screen_power(on)) {
        if (on) wake_screen();
        else sleep_screen();
    }
    hw::play_pending_beeps();  // between recordings: speaker and microphone share I2S
    Overlay incoming;
    settings::poll();
    if (take_overlay(incoming)) {
        if (incoming.kind == Overlay::Kind::None) {
            if (overlay::open()) dismiss_overlay();
        } else {
            const bool fresh = overlay::show(std::move(incoming));
            if (fresh && !settings::active()) switch_page(Page::Graphics);  // a refresh leaves you where you are
            if (current_page == Page::Graphics) drawn_version = UINT32_MAX;
            wake_screen();
            note_activity();
        }
    }
    if (console::take_show_request()) {  // a foreground run started: its console
        if (current_page != Page::Program) switch_page(Page::Program);
        program_list = false;
        output_scroll = 0;
        drawn_version = UINT32_MAX;
        wake_screen();
    }
    const console::Wait wait = console::waiting();
    if (wait == console::Wait::Line && current_page == Page::Graphics) {  // typing needs the console
        input_from_graphics = true;
        switch_page(Page::Program);
    } else if (wait == console::Wait::None && input_from_graphics) {
        input_from_graphics = false;
        if (current_page == Page::Program && overlay::open()) switch_page(Page::Graphics);
    }
}

}  // namespace

void begin() {
    init_model();
    apply_terminal_size();
    load_timeline();
    last_activity = millis();
}

void show_ready() {
    set_notice("");
    drawn_version = UINT32_MAX;
}

void enter_setup(const String& reason) { settings::open(reason); }

Page page() { return current_page; }

String action(const String& name) {
    if (name == "approve" || name == "reject" || name == "approve_all") {
        if (!has_pending()) return "no pending";
        const worker::Kind kind = name == "approve" ? worker::Kind::Approve
                                  : name == "reject" ? worker::Kind::Reject
                                                     : worker::Kind::ApproveAll;
        const uint32_t id = worker::submit({kind});
        return id ? String(id) : String("busy");
    }
    if (name == "cancel") {
        if (!worker::busy() && agent::paused()) {
            const uint32_t id = worker::submit({worker::Kind::Abort});
            return id ? String("aborted") : String("busy");
        }
        if (!worker::busy()) return "idle";
        cancel::request();
        set_stage("CANCELLING");
        return "cancelling";
    }
    if (name == "page_agent") switch_page(Page::Agent);
    else if (name == "page_ssh") switch_page(Page::Terminal);
    else if (name == "page_program") switch_page(Page::Program);
    else if (name == "page_graphics") switch_page(Page::Graphics);
    else if (name == "toggle") switch_page(next_page());
    else if (name == "cursor_up") move_cursor(-1);
    else if (name == "cursor_down") move_cursor(1);
    else if (name == "cursor_latest") cursor = -1;
    else if (name == "open_detail") open_detail();
    else if (name == "close") close_overlay();
    else if (name == "help") { view = view == View::Help ? View::List : View::Help; help_page = 0; help_cursor = 0; help_detail = false; }
    else if (name == "help_next") help_page = std::min(2, help_page + 1);
    else if (name == "detail_up" || name == "detail_down") {  // SSH page feed/raw
        worker::terminal_detail() = name == "detail_up" ? 1 : 0;
        if (!worker::busy()) worker::submit({worker::Kind::TerminalObserve});
    }
    else if (name == "wake") wake_screen();
    else if (name == "clear_history") {
        if (worker::busy()) return "busy";
        agent::clear_history();
        set_notice("对话记忆已清空");
    }
    else if (name == "mic_test") return recorder::mic_test();
    else if (name == "record_test") {
        if (worker::busy()) return "busy";
        simulated_release_ms = millis() + 2000;
        const bool ok = record(simulated_hold);
        File file = storage::fs().open("/vat/cache/ptt.pcm", FILE_READ);
        const size_t size = file ? file.size() : 0;
        if (file) file.close();
        drawn_version = UINT32_MAX;
        return "{\"ok\":" + String(ok ? "true" : "false") + ",\"bytes\":" + String(size) +
               ",\"seconds\":" + String(size / 32000.0, 2) + "}";
    }
    else return "unknown action";
    drawn_version = UINT32_MAX;
    return "ok";
}

// One key press from the web API, through the same handler as the keyboard:
// "fn+w", "enter", "del", "esc", "fn+shift" or plain text ("abc").
String press(const String& spec) {
    Keyboard_Class::KeysState state;
    String rest = spec;
    for (;;) {
        if (rest.startsWith("fn+") && rest.length() > 3) { state.fn = true; rest = rest.substring(3); }
        else if (rest == "shift" || rest.startsWith("shift+")) { state.shift = true; rest = rest.substring(rest == "shift" ? 5 : 6); }
        else break;
    }
    if (rest == "enter") state.enter = true;
    else if (rest == "del") state.del = true;
    else if (rest == "esc") state.word.push_back('`');
    else for (const char c : rest) state.word.push_back(c);
    if (!screen_on) wake_screen();  // a waking key is swallowed; injected keys must all land
    note_activity();
    dispatch_keys(state);
    render(true);
    return "ok";
}

String screen_text() {
    const Model model = snapshot();
    cJSON* root = cJSON_CreateObject();
    const char* view_name = view == View::Help ? "help" : view == View::Detail ? "detail"
                            : secret_active(model) ? "secret" : "list";
    static const char* const kPageIds[] = {"agent", "ssh", "program", "graphics"};
    cJSON_AddStringToObject(root, "page", kPageIds[static_cast<int>(current_page)]);
    cJSON_AddStringToObject(root, "view", view_name);
    const char* mascot_name = mascot::describe();  // read-only: this runs on the web task
    cJSON_AddStringToObject(root, "mascot", mascot_live || strcmp(mascot_name, "off") == 0 ? mascot_name : "hidden");
    String header = current_page == Page::Terminal ? "SSH " + model.terminal_title : String(page_name(current_page));
    if (current_page == Page::Program) header += " " + console::head().name;
    cJSON_AddStringToObject(root, "header", header.c_str());
    cJSON_AddStringToObject(root, "notice", model.notice.c_str());  // the bottom line ("" when none)
    String body;
    if (current_page == Page::Terminal) {
        body = model.terminal_text;
    } else if (current_page == Page::Program) {
        if (program_list) {
            for (const auto& item : program_items) body += String(item.app ? "[应用] " : "[程序] ") + item.name + "\n";
        } else {
            for (const auto& line : console::view().lines) body += line.text + "\n";
        }
    } else if (view == View::Detail) {
        for (const auto& line : detail_lines) body += line.first + "\n";
    } else {
        for (const auto& e : model.timeline) body += e.time + " " + e.text + " | " + e.summary + "\n";
    }
    cJSON_AddStringToObject(root, "body", body.c_str());
    cJSON_AddNumberToObject(root, "entries", model.timeline.size());
    cJSON_AddNumberToObject(root, "cursor", cursor);
    cJSON_AddBoolToObject(root, "busy", model.busy);
    cJSON_AddBoolToObject(root, "setup", settings::active());
    cJSON_AddStringToObject(root, "overlay", overlay::name());
    {
        const console::Head h = console::head();
        const console::Wait w = console::waiting();
        cJSON* c = cJSON_AddObjectToObject(root, "console");
        cJSON_AddStringToObject(c, "program", h.name.c_str());
        cJSON_AddBoolToObject(c, "running", h.running);
        cJSON_AddStringToObject(c, "status", h.status.c_str());
        cJSON_AddStringToObject(c, "wait", w == console::Wait::Line ? "line" : w == console::Wait::Key ? "key" : "");
        cJSON_AddBoolToObject(c, "list", program_list);
    }
    cJSON_AddNumberToObject(root, "canvas_frame", overlay::canvas_frame());
    cJSON_AddStringToObject(root, "canvas_error", overlay::canvas_error());
    settings::describe(root);
    cJSON_AddBoolToObject(root, "screen_on", screen_on);
    cJSON* pending = cJSON_AddArrayToObject(root, "pending");
    for (const auto& item : model.pending) cJSON_AddItemToArray(pending, cJSON_CreateString(item.summary.c_str()));
    cJSON_AddStringToObject(root, "secret_prompt",
                            model.pending.empty() ? "" : model.pending.front().secret_prompt.c_str());
    cJSON_AddBoolToObject(root, "terminal_password", model.terminal_password);
    cJSON_AddStringToObject(root, "draft", model.terminal_password ? "" : model.terminal_draft.c_str());
    cJSON_AddNumberToObject(root, "help_page", view == View::Help ? help_page + 1 : 0);
    const String json = json_print(root);
    cJSON_Delete(root);
    return json;
}

void loop() {
    const bool button = M5Cardputer.BtnA.wasPressed() && !settings::active();
    if (button) {  // round the pages; a picture stays open on the graphics page
        wake_screen();
        switch_page(next_page());
    }
    handle_keys();
    apply_hardware_requests();
    hw::loop();
    timer::loop();
    registry::loop();
    const auto& cfg = config::get();
    if (screen_on && cfg.screen_timeout_seconds > 0 && !worker::busy() && !has_pending() &&
        millis() - last_activity > cfg.screen_timeout_seconds * 1000UL) {
        sleep_screen();
    }
    if (!settings::active() && current_page == Page::Terminal && screen_on && !worker::busy() &&
        millis() - last_terminal_refresh > kTerminalRefreshMs && draft_empty()) {
        last_terminal_refresh = millis();
        worker::submit({worker::Kind::TerminalObserve});
    }
    if (worker::busy()) note_activity();
    render();
    // A program saved or removed while the page is open (the agent wrote one): the
    // list was only checked when the page was entered.
    if (current_page == Page::Program && program_items_loaded && programs::generation() != program_items_gen) {
        program_items_stale = true;
    }
    // The program list, once the page (from the cache) is on screen.
    if (current_page == Page::Program && program_list && program_items_stale && program_layout != -1 &&
        !settings::active()) {
        refresh_program_items();
        drawn_version = UINT32_MAX;
    }
}

}  // namespace vat::ui
