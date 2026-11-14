#pragma once

#include <Arduino.h>

#include <vector>

#include "ui_canvas.h"
#include "agent.h"

namespace vat::ui {

// Btn0 goes round Agent, SSH, the program page (a program's text I/O) and
// the graphics page (any picture: display, panels, canvases; only while one is open).
enum class Page : uint8_t { Agent, Terminal, Program, Graphics };

enum class Status : uint8_t { Running, Done, Failed, Confirm, Secret, Cancelled, Text };

// One line of the Agent timeline: one utterance and what came of it.  The
// full detail (raw output, every step) lives in /vat/memory/timeline.jsonl
// and is read only when the detail view opens.
struct Entry {
    uint32_t id = 0;
    String time;      // "23:41" or "--:--" before NTP sync
    String text;      // what the user said (trimmed)
    Status status = Status::Running;
    String summary;   // one or two sentences
    String stage;     // running: STT / THINK n / RUN x / SUMMARY
    int step = 0;     // running/confirm: current step number
    unsigned long started_ms = 0;
    uint32_t elapsed_ms = 0;
};

// Everything the screen shows.  The worker mutates it through the functions
// below; only the UI task draws.  All access is serialised by one mutex.
struct Model {
    std::vector<Entry> timeline;  // oldest first, at most 20
    String notice;                // transient idle message ("录音太短…")
    bool notice_error = false;
    bool busy = false;
    String stage;
    unsigned long stage_started_ms = 0;
    std::vector<agent::Pending> pending;
    String terminal_text = "没有活动终端";
    String terminal_title = "未连接";
    bool terminal_password = false;  // remote is asking for a password
    String terminal_draft;
    String agent_draft;  // Agent page: spoken segments waiting for Enter
    uint32_t version = 0;
};

void init_model();
Model snapshot();
uint32_t version();
bool is_busy();
bool draft_empty();
bool terminal_password();

// Busy flags (the Agent page shows progress on the running entry).
void set_busy(const String& stage);
void set_stage(const String& stage);
void set_quiet_busy(bool busy);  // background terminal work
void set_idle();
void set_idle_if_quiet();  // clear a busy flag left by a job that made no entry

// Timeline.
uint32_t entry_begin(const String& text);
void entry_retext(const String& text);  // transcript arrived after the entry was created
void entry_update(Status status, const String& summary);
void entry_finish(Status status, const String& summary, const String& detail, const String& steps_json);
// The newest entry's steps as they happen (agent::Progress::step), at most 16;
// entry_finish keeps them in the record ("live").  Each is its kind letter
// followed by the text.
void entry_step(char kind, const String& text);
std::vector<String> live_steps(uint32_t id);  // empty unless they are `id`'s
uint32_t live_version();  // changes with every step and when the entry finishes or waits
void load_timeline();
String entry_reply(uint32_t id);  // the whole reply when longer than the preview, else ""
void set_notice(const String& text, bool error = false);
// Detail of one entry from storage: JSON {text, summary, detail, steps, ...}.
String entry_detail(uint32_t id);

void set_pending(const std::vector<agent::Pending>& pending);
bool has_pending();

void set_terminal(const String& title, const String& text, bool password_prompt = false);
void set_draft(const String& text);
String take_draft();
void append_agent_draft(const String& text);
String take_agent_draft();
void set_agent_draft(const String& text);

// Hardware requests from the worker, applied by the UI task.
void post_brightness(int value);
void post_screen_power(bool on);
void post_beep(int count = 1);
bool take_brightness(int& value);
bool take_screen_power(bool& on);
bool take_beep(int& count);

// A picture for the graphics page (display package, ui panels and canvases).
// Kind None closes an open one.
struct Overlay {
    enum class Kind : uint8_t { None, Text, Qr, Image, Panel, Canvas };
    Kind kind = Kind::None;
    String text;                 // Text: what to show; Qr: the encoded data
    std::vector<uint8_t> image;  // Image: PNG / JPG / BMP bytes
    String image_path;           // Image: read again when the page is drawn again
    // Panel (ui package): an interactive page; choosing emits ui://panel/<panel>.
    String panel;
    String title;
    std::vector<String> lines;
    std::vector<String> choices;
    // Canvas (ui package): drawing operations; `image` holds a drawImage file.
    std::vector<ui_canvas::Op> canvas;
    std::vector<std::vector<uint8_t>> blobs;  // bitmaps, pixels and images of its M5GFX calls (gfx_exec::prepare)
    int image_x = 0;
    int image_y = 0;
    uint32_t frame_ms = 0;  // animation: 0 = still
    uint32_t frames = 0;    // 0 = until closed
    int step = 1;           // {t} = frame × step
    String vars_json;       // canvas variables and the Lua update run before each frame
    String update;
    // Bring the graphics page up even when this is a refresh of the overlay on
    // it (the first frame of a program's run; gfx://frame).
    bool raise = false;
};
void post_overlay(Overlay&& overlay);
bool take_overlay(Overlay& overlay);
bool overlay_waiting();  // posted, not yet taken by the UI task (gfx.present paces itself on it)
String open_panel();    // name of the panel or canvas on screen, "" when none
// The last choice made on each panel (kept until restart).
void set_panel_choice(const String& panel, const String& choice);
String panel_choice(const String& panel);
void set_overlay_kind(const String& kind);

}  // namespace vat::ui
