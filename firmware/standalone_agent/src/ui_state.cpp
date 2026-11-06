#include "ui_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include <algorithm>

#include "common.h"
#include "storage.h"

namespace vat::ui {
namespace {

constexpr std::size_t kTimelineLimit = 20;
constexpr char kTimelineFile[] = "/vat/memory/timeline.jsonl";

Model model;
SemaphoreHandle_t mutex = nullptr;
uint32_t next_entry_id = 1;
int brightness_request = -1;
int screen_request = -1;
int beep_request = 0;
Overlay overlay_request;
bool overlay_pending = false;
String overlay_shown;

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mutex); }
};

String clock_now() {
    struct tm now;
    if (!getLocalTime(&now, 0)) return "--:--";
    char text[6];
    snprintf(text, sizeof(text), "%02d:%02d", now.tm_hour, now.tm_min);
    return text;
}

const char* status_name(Status status) {
    switch (status) {
        case Status::Running: return "running";
        case Status::Done: return "done";
        case Status::Failed: return "failed";
        case Status::Confirm: return "confirm";
        case Status::Secret: return "secret";
        case Status::Cancelled: return "cancelled";
        case Status::Text: return "text";
    }
    return "done";
}

Status status_from(const String& name) {
    if (name == "failed") return Status::Failed;
    if (name == "cancelled") return Status::Cancelled;
    if (name == "text") return Status::Text;
    return Status::Done;  // a paused or running task does not survive a reboot
}

Entry* last_entry() { return model.timeline.empty() ? nullptr : &model.timeline.back(); }

}  // namespace

void init_model() {
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
}

Model snapshot() {
    Lock lock;
    return model;
}

uint32_t version() {
    Lock lock;
    return model.version;
}

bool is_busy() {
    Lock lock;
    return model.busy;
}

bool draft_empty() {
    Lock lock;
    return model.terminal_draft.isEmpty();
}

bool terminal_password() {
    Lock lock;
    return model.terminal_password;
}

void set_busy(const String& stage) {
    Lock lock;
    model.busy = true;
    model.stage = stage;
    model.stage_started_ms = millis();
    model.notice = "";
    Entry* entry = last_entry();
    if (entry && entry->status == Status::Running) entry->stage = stage;
    ++model.version;
}

void set_stage(const String& stage) {
    Lock lock;
    model.stage = stage;
    Entry* entry = last_entry();
    if (entry && (entry->status == Status::Running || entry->status == Status::Confirm ||
                  entry->status == Status::Secret)) {
        entry->status = Status::Running;
        entry->stage = stage;
        if (stage.startsWith("STEP ")) entry->step = stage.substring(5).toInt();
    }
    ++model.version;
}

void set_quiet_busy(bool busy) {
    Lock lock;
    model.busy = busy;
    if (busy) model.stage_started_ms = millis();
    ++model.version;
}

void set_idle() {
    Lock lock;
    model.busy = false;
    model.stage = "";
    ++model.version;
}

uint32_t entry_begin(const String& text) {
    Lock lock;
    Entry entry;
    entry.id = next_entry_id++;
    entry.time = clock_now();
    entry.text = utf8_head(text, 120);
    entry.started_ms = millis();
    entry.stage = model.stage;
    model.timeline.push_back(entry);
    if (model.timeline.size() > kTimelineLimit) model.timeline.erase(model.timeline.begin());
    model.notice = "";
    ++model.version;
    return entry.id;
}

void entry_retext(const String& text) {
    Lock lock;
    Entry* entry = last_entry();
    if (entry) entry->text = utf8_head(text, 120);
    ++model.version;
}

void set_idle_if_quiet() {
    Lock lock;
    Entry* entry = last_entry();
    if (!entry || entry->status != Status::Running) {
        model.busy = false;
        model.stage = "";
    }
    ++model.version;
}

void entry_update(Status status, const String& summary) {
    Lock lock;
    Entry* entry = last_entry();
    if (entry) {
        entry->status = status;
        entry->summary = utf8_head(summary, 300);
        entry->elapsed_ms = millis() - entry->started_ms;
    }
    model.busy = false;
    model.stage = "";
    ++model.version;
}

// One slot per entry id, reused every 32 entries (the timeline keeps 20).
String reply_path(uint32_t id) { return "/vat/memory/reply/" + String(id % 32) + ".txt"; }

void entry_finish(Status status, const String& summary, const String& detail, const String& steps_json) {
    Entry copy;
    {
        Lock lock;
        Entry* entry = last_entry();
        if (!entry) return;
        entry->status = status;
        entry->summary = utf8_head(summary, 300);
        entry->elapsed_ms = millis() - entry->started_ms;
        entry->stage = "";
        copy = *entry;
        model.busy = false;
        model.stage = "";
        ++model.version;
    }
    cJSON* record = cJSON_CreateObject();
    cJSON_AddNumberToObject(record, "id", copy.id);
    cJSON_AddStringToObject(record, "time", copy.time.c_str());
    cJSON_AddStringToObject(record, "text", copy.text.c_str());
    cJSON_AddStringToObject(record, "status", status_name(copy.status));
    cJSON_AddStringToObject(record, "summary", copy.summary.c_str());
    cJSON_AddNumberToObject(record, "elapsed_ms", copy.elapsed_ms);
    cJSON_AddStringToObject(record, "detail", utf8_head(detail, 1500).c_str());
    cJSON* steps = cJSON_Parse(steps_json.c_str());
    cJSON_AddItemToObject(record, "steps", steps ? steps : cJSON_CreateArray());
    storage::append_line(kTimelineFile, json_print(record));
    cJSON_Delete(record);
    // The entry keeps a 300-byte preview; a longer reply (the agent caps it at
    // 1500 bytes) goes to a small file of its own for the detail view.
    if (summary.length() > 300) {
        const String path = reply_path(copy.id);
        if (storage::ensure_parent(path)) storage::write_text(path, String(copy.id) + "\n" + utf8_head(summary, 1500));
    }
}

String entry_reply(uint32_t id) {
    const String text = storage::read_text(reply_path(id), 2048);
    const int newline = text.indexOf('\n');
    if (newline < 0 || static_cast<uint32_t>(text.substring(0, newline).toInt()) != id) return {};
    return text.substring(newline + 1);
}

void load_timeline() {
    const String tail = storage::read_tail(kTimelineFile, 12000);
    std::vector<Entry> entries;
    int start = 0;
    while (start < static_cast<int>(tail.length())) {
        int end = tail.indexOf('\n', start);
        if (end < 0) end = tail.length();
        cJSON* record = cJSON_Parse(tail.substring(start, end).c_str());
        start = end + 1;
        if (!record) continue;
        Entry entry;
        entry.id = json_int(record, "id", 0);
        entry.time = json_str(record, "time", "--:--");
        entry.text = json_str(record, "text");
        entry.status = status_from(json_str(record, "status"));
        entry.summary = json_str(record, "summary");
        entry.elapsed_ms = json_int(record, "elapsed_ms", 0);
        entries.push_back(entry);
        cJSON_Delete(record);
    }
    Lock lock;
    if (entries.size() > kTimelineLimit) entries.erase(entries.begin(), entries.end() - kTimelineLimit);
    model.timeline = entries;
    for (const auto& entry : entries) next_entry_id = std::max(next_entry_id, entry.id + 1);
    ++model.version;
}

String entry_detail(uint32_t id) {
    const String needle = "{\"id\":" + String(id) + ",";
    for (const std::size_t window : {4096u, 12288u, 24576u}) {  // newest records sit at the end
        const String tail = storage::read_tail(kTimelineFile, window);
        const int at = tail.lastIndexOf(needle);
        if (at < 0) continue;
        int end = tail.indexOf('\n', at);
        if (end < 0) end = tail.length();
        return tail.substring(at, end);
    }
    return {};
}

void set_notice(const String& text, bool error) {
    Lock lock;
    model.notice = text;
    model.notice_error = error;
    ++model.version;
}

void set_pending(const std::vector<agent::Pending>& pending) {
    Lock lock;
    model.pending = pending;
    ++model.version;
}

bool has_pending() {
    Lock lock;
    return !model.pending.empty();
}

void set_terminal(const String& title, const String& text, bool password_prompt) {
    Lock lock;
    model.terminal_title = title;
    model.terminal_text = text;
    model.terminal_password = password_prompt;
    ++model.version;
}

void set_draft(const String& text) {
    Lock lock;
    model.terminal_draft = text;
    ++model.version;
}

String take_draft() {
    Lock lock;
    String draft = model.terminal_draft;
    model.terminal_draft = "";
    ++model.version;
    return draft;
}

void post_brightness(int value) {
    Lock lock;
    brightness_request = value;
}

void post_screen_power(bool on) {
    Lock lock;
    screen_request = on ? 1 : 0;
}

void post_beep(int count) {
    Lock lock;
    beep_request = count;
}

bool take_brightness(int& value) {
    Lock lock;
    if (brightness_request < 0) return false;
    value = brightness_request;
    brightness_request = -1;
    return true;
}

bool take_screen_power(bool& on) {
    Lock lock;
    if (screen_request < 0) return false;
    on = screen_request == 1;
    screen_request = -1;
    return true;
}

bool take_beep(int& count) {
    Lock lock;
    count = beep_request;
    beep_request = 0;
    return count > 0;
}

void post_overlay(Overlay&& overlay) {
    Lock lock;
    overlay_request = std::move(overlay);
    overlay_pending = true;
}

bool take_overlay(Overlay& overlay) {
    Lock lock;
    if (!overlay_pending) return false;
    overlay = std::move(overlay_request);
    overlay_request = Overlay();
    overlay_pending = false;
    return true;
}

String overlay_kind() {
    Lock lock;
    return overlay_shown;
}

void set_overlay_kind(const String& kind) {
    Lock lock;
    overlay_shown = kind;
}

}  // namespace vat::ui
