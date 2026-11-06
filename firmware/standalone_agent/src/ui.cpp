#include "ui.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <math.h>

#include "agent.h"
#include "common.h"
#include "config.h"
#include "net.h"
#include "ops_local.h"
#include "ops_tui.h"
#include "ops_hw.h"
#include "ops_timer.h"
#include "ops_wifi.h"
#include "registry.h"
#include "storage.h"
#include "settings_page.h"
#include "ui_kit.h"
#include "ui_mascot.h"
#include "ui_overlay.h"
#include "ui_recorder.h"
#include "worker.h"

namespace vat::ui {
namespace {


constexpr uint32_t kScreenTimeouts[] = {0, 30, 60, 180, 300};
constexpr unsigned long kTerminalRefreshMs = 5000;

enum class View : uint8_t { List, Detail, Help };
enum class VoiceMode : uint8_t { Agent, Text };

Page current_page = Page::Agent;
View view = View::List;
VoiceMode voice_mode = VoiceMode::Agent;
int cursor = -1;  // index into the timeline; -1 follows the newest entry
int detail_scroll = 0;
uint32_t detail_id = 0;
std::vector<std::pair<String, uint16_t>> detail_lines;
int help_page = 0;
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
            g.setFont(&fonts::efontCN_12);
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

void wifi_bars(Gfx& g, int x, int y) {
    const bool online = net::state() == net::WifiState::Connected;
    const int rssi = net::rssi();
    const int level = !online ? 0 : rssi > -60 ? 3 : rssi > -75 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        const int h = 3 + i * 2;
        const uint16_t color = !online ? kDanger : i < level ? kAccent : kMuted;
        g.fillRect(x + i * 4, y + 9 - h, 3, h, color);
    }
}

void top_bar(Gfx& g, const Model& model) {
    g.fillRect(0, 0, kWidth, kBarHeight, kSurface);
    g.setFont(&fonts::efontCN_12);
    const bool agent = current_page == Page::Agent;
    g.setTextColor(agent ? kText : kMuted, kSurface);
    g.drawString("Agent", kPad, 2);
    const int agent_w = g.textWidth("Agent");
    g.setTextColor(!agent ? kText : kMuted, kSurface);
    g.drawString("SSH", kPad + agent_w + 10, 2);
    const int underline_x = agent ? kPad : kPad + agent_w + 10;
    const int underline_w = agent ? agent_w : g.textWidth("SSH");
    g.fillRect(underline_x, kBarHeight - 2, underline_w, 2, kAccent);

    // Right side: busy seconds, where we are, Wi-Fi.
    int right = kWidth - kPad - 12;
    wifi_bars(g, right, 3);
    right -= 6;
    String where;
    uint16_t where_color = kMuted;
    if (agent) {
        where = tui::has_current() ? tui::current().profile : config::get().default_profile;
    } else {
        where = model.terminal_title;
        const bool online = where != "未连接";
        g.fillCircle(right - 3, 7, 3, online ? kAccent : kMuted);
        right -= 10;
        where_color = online ? kText : kMuted;
    }
    g.setTextColor(where_color, kSurface);
    where = fit(g, where, 90);
    g.drawString(where, right - g.textWidth(where), 2);
    right -= g.textWidth(where) + 6;
    if (model.busy && agent) {
        const String secs = String((millis() - model.stage_started_ms) / 1000) + "s";
        g.setTextColor(kAccent, kSurface);
        g.drawString(secs, right - g.textWidth(secs), 2);
    }
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

void draw_timeline(Gfx& g, const Model& model, bool mascot_on) {
    g.setFont(&fonts::efontCN_12);
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

    g.fillRect(0, kBarHeight, kWidth, top - 1 - kBarHeight, kBg);
    if (mascot_on) mascot::reset_backdrop(kBg);  // rows beside it set their own colour below
    int drawn = 0;
    for (int r = first; r < static_cast<int>(rows.size()) && r < first + visible; ++r, ++drawn) {
        const Row& row = rows[r];
        const int y = top + (r - first) * kLine;
        const Entry& e = model.timeline[row.entry];
        const bool sel = row.entry == selected && cursor >= 0;
        const bool attention = row.entry == count - 1 &&
                               (e.status == Status::Confirm || e.status == Status::Secret);
        const uint16_t bg = sel ? kSurface : kBg;
        // Rows beside the corner mascot: the background runs up to it (the mascot
        // paints the same colour behind itself, so a selected row shows through),
        // the text stops short of it.
        const int right = mascot_on && y - 1 + kLine > mascot::kTop ? mascot::kLeft - 2 : kWidth;
        const bool short_row = right < kWidth;
        g.fillRect(0, y - 1, short_row ? mascot::kLeft : kWidth, kLine, bg);  // each row clears itself
        if (short_row && y - 1 < mascot::kTop) {  // the slice of the row above the mascot runs to the edge
            g.fillRect(mascot::kLeft, y - 1, mascot::kSize, mascot::kTop - (y - 1), bg);
        }
        if (short_row) {
            mascot::set_backdrop(y - 1, kLine, bg);
            g.setClipRect(0, y - 1, right, kLine);
        }
        if (sel || attention) g.fillRect(0, y - 1, 2, kLine, attention ? kWarn : kAccent);
        if (row.head) {
            g.setTextColor(kMuted, bg);
            g.drawString(e.time, kPad, y);
            g.setTextColor(e.status == Status::Cancelled ? kMuted : kText, bg);
            g.drawString(short_row ? fit(g, row.text, right - kPad * 2 - 44 - 14) : row.text, kPad + 44, y);
            icon(g, right - kPad - 10, y + 1, e.status, sel);
        } else if (row.text == "\x01stage") {
            draw_stage(g, kPad + row.indent, y, e, bg);
        } else {
            g.setTextColor(row.color, bg);
            g.drawString(short_row ? fit(g, row.text, right - kPad - row.indent) : row.text, kPad + row.indent, y);
        }
        if (short_row) g.clearClipRect();
    }

    const int rows_end = top - 1 + drawn * kLine;  // clear what the rows did not cover
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
    g.setFont(&fonts::efontCN_12);
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
    g.setFont(&fonts::efontCN_12);
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
    g.setFont(&fonts::efontCN_12);
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
    g.setFont(&fonts::efontCN_12);
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
        row("Enter", "按住说话", kMuted);
        row("Btn0", "切换 Agent / SSH", kMuted);
        row("; .", "上下选择   / 详情   , 返回", kMuted);
        row("Y N A", "执行 / 取消 / 后续都执行", kMuted);
        row("`", "取消当前任务", kMuted);
        row("Fn+Tab", "只转写模式   Fn+W WiFi", kMuted);
        row("Fn+Space", "息屏   Opt+, . 息屏时间", kMuted);
        return;
    }
    const auto hints = registry::hints();
    for (const auto& hint : hints) {
        const bool setting = !hint.value.isEmpty();
        if ((help_page == 1) != setting) continue;
        if (y > kHeight - kLine) break;
        g.setTextColor(kText, kBg);
        g.drawString(fit(g, hint.name, 52), kPad, y);
        int x = kPad + 56;
        if (setting) {
            g.setTextColor(kAccent, kBg);
            const String value = fit(g, hint.value, 60);
            g.drawString(value, x, y);
            x += std::max<int>(64, g.textWidth(value) + 6);
        }
        g.setTextColor(kMuted, kBg);
        g.drawString(fit(g, "“" + hint.example + "”", kWidth - x - kPad), x, y);
        y += kLine;
    }
}

// ---------------------------------------------------------------- SSH page

void draw_terminal(Gfx& g, const Model& model) {
    const TermFont tf = term_font();
    g.setFont(tf.font);
    const int top = kBarHeight + 2;
    String body = model.terminal_text;
    if (model.terminal_password) {
        String masked;
        for (std::size_t i = 0; i < model.terminal_draft.length(); ++i) {
            if ((static_cast<unsigned char>(model.terminal_draft[i]) & 0xC0) != 0x80) masked += "*";
        }
        body += "\n" + masked + "_";
    } else if (!model.terminal_draft.isEmpty()) {
        body += "\n> " + model.terminal_draft + "_";
    }
    const int bottom = model.terminal_password ? kHeight - tf.line - 2 : kHeight;
    const int visible = (bottom - top) / tf.line;
    auto lines = wrap(g, body, kWidth - 4, 200);
    const int first = std::max(0, static_cast<int>(lines.size()) - visible);
    const bool offline = model.terminal_title == "未连接";
    for (int i = first; i < static_cast<int>(lines.size()); ++i) {
        g.setTextColor(offline ? kMuted : kText, kBg);
        g.drawString(lines[i], 2, top + (i - first) * tf.line);
    }
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
    if (overlay::open()) {
        overlay::draw(force);
        return;
    }
    const uint32_t current = version();
    // Mascot frames: a 32x32 push of a frame read beforehand, nothing else redrawn.
    if (!force && current == drawn_version && mascot_live && mascot::due() && mascot::load()) {
        Gfx& g = M5Cardputer.Display;
        g.startWrite();
        mascot::push(g);
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
    const bool timeline = view != View::Help && current_page != Page::Terminal && view != View::Detail;
    const bool secret = timeline && secret_active(model);
    mascot_live = timeline && !secret && mascot::enabled();
    if (mascot_live) {  // the card is read before the display transaction starts
        mascot::request(mascot_state(model));
        mascot_live = mascot::load();
    }
    // A full-screen sprite would cost 64 KB of RAM; draw directly.  The
    // timeline clears row by row instead of blanking the whole screen first.
    g.startWrite();
    if (!timeline) g.fillScreen(kBg);
    g.setTextWrap(false, false);
    top_bar(g, model);
    if (view == View::Help) {
        draw_help(g);
    } else if (current_page == Page::Terminal) {
        draw_terminal(g, model);
    } else if (view == View::Detail) {
        draw_detail(g, model);
    } else {
        draw_timeline(g, model, mascot_live);
        if (secret) draw_secret(g, model);
    }
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
    current_page = next;
    view = View::List;
    drawn_version = UINT32_MAX;
    if (next == Page::Terminal && !worker::busy() && draft_empty()) {
        worker::submit({worker::Kind::TerminalObserve});
        last_terminal_refresh = millis();
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
    secret_buffer = String();  // the copy lives only inside the job
    secret_reveal = false;
    submit_or_busy(job);
    job.text = String();
}

void dispatch_keys(const Keyboard_Class::KeysState& state);

void handle_keys() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    dispatch_keys(M5Cardputer.Keyboard.keysState());
}

void dispatch_keys(const Keyboard_Class::KeysState& state) {
    if (!screen_on) {
        wake_screen();
        render(true);
        if (!state.enter) return;
    }
    note_activity();
    if (overlay::open() && !settings::active()) {
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
        else if (has_char(state, 'h')) { view = view == View::Help ? View::List : View::Help; help_page = 0; }
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
        if (has_char(state, ',')) help_page = std::max(0, help_page - 1);
        if (has_char(state, '/')) help_page = std::min(2, help_page + 1);
        drawn_version = UINT32_MAX;
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
    // SSH page: the keyboard types into the remote terminal.
    if (busy) return;
    if (state.ctrl && has_char(state, 'c')) { send_terminal("", {"ctrl_c"}, false); return; }
    if (esc) { send_terminal("", {"esc"}, false); return; }
    if (state.tab) { send_terminal("", {"tab"}, false); return; }
    String draft = model.terminal_draft;
    if (state.enter) {
        take_draft();
        send_terminal(draft, {}, true);  // a password draft is never displayed or logged
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
    if (take_overlay(incoming)) {
        if (incoming.kind == Overlay::Kind::None) {
            if (overlay::open()) dismiss_overlay();
        } else {
            overlay::show(std::move(incoming));
            wake_screen();
            note_activity();
        }
    }
}

}  // namespace

void begin() {
    init_model();
    apply_terminal_size();
    load_timeline();
    last_activity = millis();
}

void show_boot(const String& title, const String& message, uint16_t color) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(kBg);
    d.setFont(&fonts::efontCN_12);
    d.setTextColor(kAccent, kBg);
    d.drawString(title, kPad, 6);
    d.setTextColor(color == TFT_YELLOW ? kWarn : color == TFT_RED ? kDanger : color, kBg);
    d.drawString(message, kPad, 6 + kLine + 4);
}

void show_ready() {
    set_notice("");
    drawn_version = UINT32_MAX;
}

void enter_setup(const String& reason) { settings::open(reason); }

bool in_setup() { return settings::active(); }

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
    else if (name == "toggle") switch_page(current_page == Page::Agent ? Page::Terminal : Page::Agent);
    else if (name == "cursor_up") move_cursor(-1);
    else if (name == "cursor_down") move_cursor(1);
    else if (name == "cursor_latest") cursor = -1;
    else if (name == "open_detail") open_detail();
    else if (name == "close") close_overlay();
    else if (name == "help") { view = view == View::Help ? View::List : View::Help; help_page = 0; }
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
    cJSON_AddStringToObject(root, "page", current_page == Page::Agent ? "agent" : "ssh");
    cJSON_AddStringToObject(root, "view", view_name);
    const char* mascot_name = mascot::describe();  // read-only: this runs on the web task
    cJSON_AddStringToObject(root, "mascot", mascot_live || strcmp(mascot_name, "off") == 0 ? mascot_name : "hidden");
    String header = current_page == Page::Agent ? String("Agent") : "SSH " + model.terminal_title;
    cJSON_AddStringToObject(root, "header", header.c_str());
    String body;
    if (current_page == Page::Terminal) {
        body = model.terminal_text;
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
    if (button && overlay::open()) {
        dismiss_overlay();
    } else if (button) {
        wake_screen();
        switch_page(current_page == Page::Agent ? Page::Terminal : Page::Agent);
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
}

}  // namespace vat::ui
