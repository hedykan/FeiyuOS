#include "console.h"

#include <atomic>
#include <mutex>

#include "worker.h"

namespace vat::console {
namespace {

std::mutex lock;
Buffer buffer;
View state;              // everything but the lines (buffer holds them)
String preset;           // the run's stdin text, not read yet
bool preset_given = false;
String answer;           // a submitted line / key, for the waiting worker
bool answered = false;
bool show_request = false;
std::atomic<uint32_t> changes{0};

void changed() { ++changes; }

// One UTF-8 character from the front of `text`.
String take_char(String& text) {
    std::size_t n = 1;
    const uint8_t c = static_cast<uint8_t>(text[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    const String out = text.substring(0, n);
    text = text.substring(n);
    return out;
}

// Waits for the UI's answer; Cancelled on `; Timeout after timeout_ms (0: none).
Got wait_answer(Wait kind, const String& prompt, uint32_t timeout_ms, String& out) {
    {
        std::lock_guard<std::mutex> guard(lock);
        state.wait = kind;
        state.prompt = prompt;
        state.input = String();
        answered = false;
        changed();
    }
    const uint32_t since = millis();
    Got got = Got::Timeout;
    while (true) {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (answered) {
                out = answer;
                answered = false;
                got = Got::Value;
                break;
            }
        }
        if (cancel::requested()) {
            got = Got::Cancelled;
            break;
        }
        if (timeout_ms && millis() - since >= timeout_ms) break;
        worker::keep_alive();  // waiting for the user is not a stuck job
        delay(20);
    }
    std::lock_guard<std::mutex> guard(lock);
    state.wait = Wait::None;
    state.prompt = String();
    state.input = String();
    changed();
    return got;
}

}  // namespace

void start(const String& name, const String& stdin_text) {
    std::lock_guard<std::mutex> guard(lock);
    buffer.clear();
    state = View();
    state.name = name;
    state.running = true;
    state.started = millis();
    preset = stdin_text;
    preset_given = !stdin_text.isEmpty();
    show_request = true;
    changed();
}

void finish(const String& status, bool failed) {
    std::lock_guard<std::mutex> guard(lock);
    state.running = false;
    state.failed = failed;
    state.status = status;
    state.wait = Wait::None;
    preset = String();
    preset_given = false;
    changed();
}

bool active() {
    std::lock_guard<std::mutex> guard(lock);
    return state.running;
}

void write(const String& text, bool error) {
    std::lock_guard<std::mutex> guard(lock);
    buffer.write(text, error);
    changed();
}

void clear() {
    std::lock_guard<std::mutex> guard(lock);
    buffer.clear();
    changed();
}

String text() {
    std::lock_guard<std::mutex> guard(lock);
    String out;
    for (const auto& line : buffer.lines()) out += line.text + "\n";
    return out;
}

Got read_line(const String& prompt, String& line) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (preset_given) {  // the run's stdin, like a pipe: End once it is used up
            if (preset.isEmpty()) return Got::End;
            const int nl = preset.indexOf('\n');
            line = nl < 0 ? preset : preset.substring(0, nl);
            preset = nl < 0 ? String() : preset.substring(nl + 1);
            buffer.write(prompt + line + "\n");
            changed();
            return Got::Value;
        }
    }
    const Got got = wait_answer(Wait::Line, prompt, 0, line);
    if (got == Got::Value) write(prompt + line + "\n");
    return got;
}

Got read_key(uint32_t timeout_ms, String& key) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (preset_given) {
            if (preset.isEmpty()) return Got::End;
            key = take_char(preset);
            if (key == "\n") key = "enter";
            return Got::Value;
        }
    }
    return wait_answer(Wait::Key, String(), timeout_ms, key);
}

uint32_t version() { return changes.load(); }

View view() {
    std::lock_guard<std::mutex> guard(lock);
    View v = state;
    v.lines = buffer.lines();
    return v;
}

Head head() {
    std::lock_guard<std::mutex> guard(lock);
    Head h;
    h.name = state.name;
    h.running = state.running;
    h.failed = state.failed;
    h.status = state.status;
    h.started = state.started;
    return h;
}

Wait waiting() {
    std::lock_guard<std::mutex> guard(lock);
    return state.wait;
}

bool take_show_request() {
    std::lock_guard<std::mutex> guard(lock);
    const bool was = show_request;
    show_request = false;
    return was;
}

void type(const String& text) {
    std::lock_guard<std::mutex> guard(lock);
    if (state.wait != Wait::Line || state.input.length() + text.length() > 200) return;
    state.input += text;
    changed();
}

void erase() {
    std::lock_guard<std::mutex> guard(lock);
    if (state.wait != Wait::Line) return;
    std::size_t cut = state.input.length();  // back over one UTF-8 character
    while (cut > 0 && (static_cast<uint8_t>(state.input[cut - 1]) & 0xC0) == 0x80) --cut;
    if (cut > 0) --cut;
    state.input = state.input.substring(0, cut);
    changed();
}

void submit() {
    std::lock_guard<std::mutex> guard(lock);
    if (state.wait != Wait::Line) return;
    answer = state.input;
    answered = true;
}

void key(const String& name) {
    std::lock_guard<std::mutex> guard(lock);
    if (state.wait != Wait::Key) return;
    answer = name;
    answered = true;
}

}  // namespace vat::console
