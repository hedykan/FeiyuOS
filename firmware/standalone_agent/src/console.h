#pragma once

#include <Arduino.h>

#include <vector>

#include "console_buffer.h"

// The program page's console: the text I/O of the program running in the
// foreground (started by the user: a tool call or the program page; not a
// trigger).  The worker writes and waits for input; the UI task draws and
// types.  One foreground run at a time (the worker runs one job).
namespace vat::console {

enum class Wait : uint8_t { None, Line, Key };

struct View {
    std::vector<Line> lines;
    String name;        // the program, "" before any run
    bool running = false;
    bool failed = false;
    String status;      // after the run: 完成 / 失败：… / 已中断
    Wait wait = Wait::None;
    String prompt;      // what a line is asked with
    String input;       // typed so far
    uint32_t started = 0;
};

// ---- worker side
void start(const String& name, const String& stdin_text);  // a foreground run starts
void finish(const String& status, bool failed);
bool active();                                   // a foreground run is on
void write(const String& text, bool error = false);
void clear();
String text();                                   // the run's output (for its log)
enum class Got : uint8_t { Value, End, Cancelled, Timeout };
// A line typed on the page (or from the run's stdin text; End once that is used
// up).  Feeds the job watchdog while waiting.
Got read_line(const String& prompt, String& line);
// One key ("a", "enter", "del", "tab", "space"; ; . , / are up down left right
// by convention); timeout_ms 0 = until one comes.
Got read_key(uint32_t timeout_ms, String& key);

// ---- UI task
uint32_t version();      // changes with every change worth a redraw
View view();
struct Head {            // view() without the lines (the top bar, every second)
    String name;
    bool running = false;
    bool failed = false;
    String status;
    uint32_t started = 0;
};
Head head();
Wait waiting();
bool take_show_request();  // a run started: show the program page (once)
void type(const String& text);
void erase();
void submit();             // Enter on a line
void key(const String& name);

}  // namespace vat::console
