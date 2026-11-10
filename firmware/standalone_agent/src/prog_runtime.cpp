#include "prog_runtime.h"

#include <atomic>

#include <cJSON.h>
#include <time.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "agent.h"
#include "common.h"
#include "console.h"
#include "lua_meta.h"
#include "lua_vm.h"
#include "ops_timer.h"
#include "registry.h"
#include "storage.h"
#include "ui_state.h"
#include "worker.h"

namespace vat::programs {
namespace {

constexpr char kDir[] = "/vat/things/lua";
constexpr char kIndex[] = "/vat/things/lua/index.json";
constexpr char kApproved[] = "/vat/things/lua/approved.json";
constexpr char kTriggers[] = "/vat/things/lua/triggers.json";
constexpr char kStats[] = "/vat/prog/stats.json";
constexpr char kLegacyDir[] = "/vat/things/prog/programs";      // v0.6.2 JSON programs
constexpr char kLegacyTriggers[] = "/vat/things/prog/triggers.json";
constexpr std::size_t kMaxFiles = 64;
constexpr std::size_t kMaxTriggers = 32;
constexpr std::size_t kMaxQueue = 8;

constexpr long kMinInterval = 10;   // seconds: time://every/<duration>
constexpr int kFailuresToPause = 5;  // in a row: the trigger pauses itself

struct Trigger {
    int id = 0;
    String when;
    String run;  // "prog://名字?参数"
    bool paused = false;
    bool armed = false;
    String paused_why;  // "" or why it paused itself (kept in triggers.json)
    // Since boot, not saved (an interval trigger runs every few seconds):
    uint32_t next_ms = 0;  // interval: when it is due
    time_t last_run = 0;
    bool last_ok = true;
    String last_note;      // the result's or error's first line
    int runs = 0;
    int failures = 0;      // in a row
    int skipped = 0;       // ticks skipped because the device was busy
    bool queued = false;   // a run is waiting or running
};

// time://every/30s (an interval: seconds), 0 for the other events, -1 when
// it looks like an interval but is not one ("every/5x").
long interval_of(const String& when) {
    if (!when.startsWith("time://every/")) return 0;
    const String value = when.substring(strlen("time://every/"));
    if (value.indexOf(':') >= 0) return 0;  // every/07:00: a daily time
    return timer::duration_seconds(value);
}

struct Item {  // a queued run
    String label;
    int trigger = 0;  // the trigger that queued it (0: none)
    String program;
    String query;
    String data;  // event data (JSON), "" when none
};

std::vector<Trigger> triggers;
std::vector<Item> queue;
std::vector<std::pair<String, String>> events_in;  // target, data
std::vector<String> recent;
std::mutex lock;
bool loaded = false;
uint32_t last_arm = 0;

// ---------------------------------------------------------------- helpers

String clock_text(time_t t, const char* format) {
    if (t < 1700000000) return "--";
    struct tm tm;
    localtime_r(&t, &tm);
    char text[24];
    strftime(text, sizeof(text), format, &tm);
    return text;
}

cJSON* read_json(const String& path, std::size_t max = 32768) {
    return cJSON_Parse(storage::read_text(path, max).c_str());
}

std::atomic<uint32_t> changes{0};  // generation(): the index and confirmations

bool write_json(const String& path, const cJSON* value) {
    if (path == kIndex || path == kApproved) ++changes;
    return storage::ensure_parent(path) && storage::write_text(path, json_print(value));
}

// Uses to confirm, or a package: its usage text goes into the agent's prompt,
// so every package is enabled by the user once, uses or not.
bool must_confirm(const lua_meta::Header& h) { return lua_meta::needs_confirm(h) || !h.package.isEmpty(); }

cJSON* object_at(const String& path) {
    cJSON* root = read_json(path);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

void put(cJSON* object, const char* key, cJSON* value) {
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddItemToObject(object, key, value);
}

String path_of(const String& name) { return String(kDir) + "/" + name + ".lua"; }
String old_path_of(const String& name) { return String(kDir) + "/old/" + name + ".lua"; }  // scan skips dirs

void note_recent(const String& line) {
    std::lock_guard<std::mutex> guard(lock);
    recent.push_back(clock_text(time(nullptr), "%H:%M") + " " + utf8_head(line, 120));
    if (recent.size() > 6) recent.erase(recent.begin());
}

void record_stat(const String& name, bool created, bool ok, const String& result) {
    cJSON* root = object_at(kStats);
    cJSON* entry = json_obj(root, name.c_str());
    if (!entry) entry = cJSON_AddObjectToObject(root, name.c_str());
    const double now = static_cast<double>(time(nullptr));
    if (created) {
        put(entry, "created", cJSON_CreateNumber(now));
    } else {
        put(entry, "last", cJSON_CreateNumber(now));
        put(entry, "ok", cJSON_CreateBool(ok));
        put(entry, "result", cJSON_CreateString(utf8_head(result, 80).c_str()));
    }
    write_json(kStats, root);
    cJSON_Delete(root);
}

void forget(const char* path, const String& name) {
    cJSON* root = object_at(path);
    cJSON_DeleteItemFromObjectCaseSensitive(root, name.c_str());
    write_json(path, root);
    cJSON_Delete(root);
}

String describe_when(const String& when) {
    const int slash = when.indexOf('/', 7);
    const String kind = when.startsWith("time://") && slash > 0 ? when.substring(7, slash) : String();
    const String value = slash > 0 ? when.substring(slash + 1) : String();
    if (kind == "every" && value.indexOf(':') < 0) return "每隔 " + value;
    if (kind == "every") return "每天 " + value;
    if (kind == "weekday") return "工作日 " + value;
    if (kind == "weekend") return "周末 " + value;
    if (kind == "at") return value.substring(5, 16) + " 一次";
    if (when == "device://boot") return "每次开机";
    return when;
}

// "a=1&b=%E6%B7%B1" -> {"a": "1", "b": "深"}
cJSON* parse_query(const String& query) {
    cJSON* out = cJSON_CreateObject();
    std::size_t at = 0;
    while (at < query.length()) {
        int end = query.indexOf('&', at);
        if (end < 0) end = query.length();
        const String pair = query.substring(at, end);
        at = end + 1;
        const int eq = pair.indexOf('=');
        if (pair.isEmpty() || eq == 0) continue;
        const String key = url_decode(eq < 0 ? pair : pair.substring(0, eq));
        const String value = eq < 0 ? String() : url_decode(pair.substring(eq + 1));
        put(out, key.c_str(), cJSON_CreateString(value.c_str()));
    }
    return out;
}

// ---------------------------------------------------------------- files, index, confirmations

enum class State { Enabled, Pending, Off, Broken };

struct Entry {
    String name;
    lua_meta::Header header;
    String problem;  // the header's error, if any
    std::size_t size = 0;
    State state = State::Broken;
};

State state_of(const String& name, const lua_meta::Header& h, const String& problem) {
    if (!problem.isEmpty()) return State::Broken;
    cJSON* root = object_at(kApproved);
    const cJSON* entry = json_obj(root, name.c_str());
    const bool off = json_bool(entry, "off", false);
    // valueint saturates at INT_MAX: a digest above 2^31 must be read as a double
    const cJSON* d = cJSON_GetObjectItemCaseSensitive(entry, "digest");
    const uint32_t confirmed = cJSON_IsNumber(d) ? static_cast<uint32_t>(d->valuedouble) : 0;
    cJSON_Delete(root);
    if (off) return State::Off;
    if (must_confirm(h) && confirmed != lua_meta::digest(h)) return State::Pending;
    return State::Enabled;
}

const char* state_word(State s) {
    switch (s) {
        case State::Enabled: return "已启用";
        case State::Pending: return "待确认";
        case State::Off: return "已停用";
        default: return "声明有误";
    }
}

bool load_entry(const String& name, Entry& e) {
    const String path = path_of(name);
    if (!storage::exists(path)) return false;
    e = Entry();
    e.name = name;
    e.size = storage::size(path);
    e.problem = lua_meta::parse(storage::read_text(path, 4096), e.header);
    e.state = state_of(name, e.header, e.problem);
    return true;
}

void confirm(const String& name, const lua_meta::Header& h) {
    cJSON* root = object_at(kApproved);
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "digest", static_cast<double>(lua_meta::digest(h)));
    put(root, name.c_str(), entry);
    write_json(kApproved, root);
    cJSON_Delete(root);
}

void set_off(const String& name, bool off) {
    cJSON* root = object_at(kApproved);
    cJSON* entry = json_obj(root, name.c_str());
    if (!entry) entry = cJSON_AddObjectToObject(root, name.c_str());
    put(entry, "off", cJSON_CreateBool(off));
    write_json(kApproved, root);
    cJSON_Delete(root);
}

// Every .lua on the card, read from the index (built by scan()).
std::vector<Entry> entries() {
    std::vector<Entry> out;
    cJSON* index = read_json(kIndex);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, index) {
        Entry e;
        if (load_entry(json_str(item, "name"), e)) out.push_back(e);
    }
    cJSON_Delete(index);
    return out;
}

void write_index(const std::vector<String>& names) {
    cJSON* list = cJSON_CreateArray();
    for (const auto& n : names) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", n.c_str());
        cJSON_AddItemToArray(list, item);
    }
    write_json(kIndex, list);
    cJSON_Delete(list);
}

std::vector<String> index_names() {
    std::vector<String> names;
    cJSON* index = read_json(kIndex);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, index) names.push_back(json_str(item, "name"));
    cJSON_Delete(index);
    return names;
}

void index_add(const String& name) {
    auto names = index_names();
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    write_index(names);
}

void index_remove(const String& name) {
    auto names = index_names();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    write_index(names);
}

String signature(const String& name, const lua_meta::Header& h) {
    String sig = name;
    if (!h.inputs.empty()) {
        sig += "(";
        for (std::size_t i = 0; i < h.inputs.size(); ++i) {
            if (i) sig += ", ";
            sig += h.inputs[i].name;
            if (!h.inputs[i].required) sig += "=" + h.inputs[i].fallback;
        }
        sig += ")";
    }
    return sig;
}

String users_of(const String& name) {  // triggers that run `name`
    std::lock_guard<std::mutex> guard(lock);
    String out;
    for (const auto& t : triggers) {
        String run = t.run.substring(7);
        const int q = run.indexOf('?');
        if (q >= 0) run = run.substring(0, q);
        if (url_decode(run) == name) out += String(out.isEmpty() ? "" : "、") + "触发 #" + String(t.id);
    }
    return out;
}

// ---------------------------------------------------------------- running

struct Outcome {
    bool ok = false;
    String result;
    String output;
    String error;
};

// Started with Enter on the program page: missing inputs are asked for on the
// console.  A tool call reports them instead (the agent asks the user).
bool ask_inputs = false;

Outcome run_checked(const String& name, const String& query, const String& event_json, bool foreground) {
    Outcome o;
    Entry e;
    if (!load_entry(name, e)) {
        o.error = "没有程序「" + name + "」（read prog://list 查看）";
        return o;
    }
    if (!e.header.package.isEmpty()) {
        o.error = "「" + name + "」是扩展包，不能直接运行";
        return o;
    }
    if (e.state == State::Broken) o.error = "「" + name + "」的声明有误：" + e.problem;
    else if (e.state == State::Off) o.error = "「" + name + "」已停用（write pkg://" + name + "/enable 启用）";
    else if (e.state == State::Pending) o.error = "「" + name + "」的权限声明还没确认（重新保存它，或 write pkg://" + name + "/enable）";
    if (!o.error.isEmpty()) return o;

    cJSON* input = parse_query(query);
    for (const auto& i : e.header.inputs) {
        if (cJSON_GetObjectItemCaseSensitive(input, i.name.c_str())) continue;
        if (i.required && foreground && ask_inputs) {  // started from the program page: ask
            String value;
            const auto got = console::read_line("参数 " + i.name + "：", value);
            if (got == console::Got::Value) {
                cJSON_AddStringToObject(input, i.name.c_str(), value.c_str());
                continue;
            }
            o.error = got == console::Got::Cancelled ? String("已中断") : "缺少参数 " + i.name;
            cJSON_Delete(input);
            return o;
        }
        if (i.required) {
            o.error = "缺少参数 " + i.name + "（运行：exec prog://" + signature(name, e.header) + "）";
            cJSON_Delete(input);
            return o;
        }
        cJSON_AddStringToObject(input, i.name.c_str(), i.fallback.c_str());
    }
    if (!event_json.isEmpty()) {
        cJSON* event = cJSON_Parse(event_json.c_str());
        put(input, "event", event ? event : cJSON_CreateString(event_json.c_str()));
    }
    luavm::Options options;
    options.uses = &e.header.uses;
    options.input = input;
    options.foreground = foreground;
    luavm::Stats stats;
    o.ok = luavm::run(storage::read_text(path_of(name), lua_meta::kMaxSource), name, options, o.output, o.result, o.error,
                      stats);
    cJSON_Delete(input);
    runtime_event("prog_run", name + (o.ok ? " done" : " failed") + " peak=" + String(static_cast<unsigned>(stats.peak)) +
                                  " arena=" + String(static_cast<unsigned>(stats.arena)) + " ms=" + String(stats.ms));
    record_stat(name, false, o.ok, o.ok ? o.result : o.error);
    note_recent(name + (o.ok ? " 完成" : " 失败：" + o.error));
    return o;
}

// The outermost run decides: a tool call or the program page is in the
// foreground (the console), a trigger in the background; programs it calls
// run the same way.  One worker task: plain variables.
int run_level = 0;
bool run_foreground = false;

Outcome run_program(const String& name, const String& query, const String& event_json, bool foreground,
                    const String& stdin_text = String()) {
    const bool top = run_level == 0;
    if (top) run_foreground = foreground;
    const bool fg = run_foreground;
    if (top && fg) console::start(name, stdin_text);
    ++run_level;
    Outcome o = run_checked(name, query, event_json, fg);
    --run_level;
    if (top && fg) {
        const bool stopped = cancel::requested();
        if (!o.ok) console::write(o.error + "\n", true);
        console::finish(o.ok ? (o.result.isEmpty() ? String("完成") : "完成：" + utf8_head(o.result, 120))
                             : stopped ? String("已中断") : String("失败"),
                        !o.ok);
        storage::write_text(String(kDir) + "/logs/" + name + ".txt", console::text());
    }
    return o;
}

// ---------------------------------------------------------------- triggers

void save_triggers() {  // caller holds `lock`
    cJSON* list = cJSON_CreateArray();
    for (const auto& t : triggers) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", t.id);
        cJSON_AddStringToObject(item, "when", t.when.c_str());
        cJSON_AddStringToObject(item, "run", t.run.c_str());
        cJSON_AddBoolToObject(item, "paused", t.paused);
        if (!t.paused_why.isEmpty()) cJSON_AddStringToObject(item, "paused_why", t.paused_why.c_str());
        cJSON_AddItemToArray(list, item);
    }
    write_json(kTriggers, list);
    cJSON_Delete(list);
}

void read_triggers() {  // caller holds `lock`
    cJSON* list = read_json(kTriggers);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        Trigger t;
        t.id = json_int(item, "id", 0);
        t.when = json_str(item, "when");
        t.run = json_str(item, "run");
        t.paused = json_bool(item, "paused", false);
        t.paused_why = json_str(item, "paused_why");
        if (t.id > 0 && !t.when.isEmpty() && t.run.startsWith("prog://")) triggers.push_back(t);
    }
    cJSON_Delete(list);
}

// v0.6.2 triggers ran one JSON step; those that ran a program become
// "prog://名字?参数", the rest are dropped (their ids go to `dropped`, noted
// by the caller once `lock` is released).  Caller holds `lock`.
void migrate_triggers(std::vector<String>& dropped) {
    if (storage::exists(kTriggers) || !storage::exists(kLegacyTriggers)) return;
    cJSON* old = read_json(kLegacyTriggers);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, old) {
        const String run = json_str(json_obj(item, "run"), "exec");
        if (!run.startsWith("prog://")) {
            dropped.push_back("#" + String(json_int(item, "id", 0)));
            continue;
        }
        Trigger t;
        t.id = json_int(item, "id", 0);
        t.when = json_str(item, "when");
        t.run = run;
        t.paused = json_bool(item, "paused", false);
        if (t.id > 0 && !t.when.isEmpty()) triggers.push_back(t);
    }
    cJSON_Delete(old);
    save_triggers();
    storage::rename(kLegacyTriggers, String(kLegacyTriggers) + ".old");
}

void load_triggers() {
    std::vector<String> dropped;
    {
        std::lock_guard<std::mutex> guard(lock);
        triggers.clear();
        migrate_triggers(dropped);
        if (triggers.empty()) read_triggers();
    }
    // note_recent() takes `lock` too: never while it is held
    for (const auto& d : dropped) note_recent("旧触发 " + d + " 不是运行程序，未迁移（见 triggers.json.old）");
}

bool one_off(const String& when) { return when.startsWith("time://at/"); }

// Arms every active trigger not armed yet; one-offs whose time passed while
// the device was off are dropped (not run late).
void arm_triggers() {
    std::vector<String> missed;
    {
        std::lock_guard<std::mutex> guard(lock);
        bool changed = false;
        for (auto it = triggers.begin(); it != triggers.end();) {
            if (it->paused || it->armed) {
                ++it;
                continue;
            }
            if (interval_of(it->when) > 0) {  // ours: due one interval from now
                it->armed = true;
                it->next_ms = millis() + interval_of(it->when) * 1000UL;
                ++it;
                continue;
            }
            String normalized;
            const String err = registry::watch(it->when, normalized);
            if (err.isEmpty()) {
                it->armed = true;
                ++it;
            } else if (err.indexOf("已经过去") >= 0 && one_off(it->when)) {
                missed.push_back("错过触发 #" + String(it->id) + "（" + describe_when(it->when) + "，关机期间）");
                it = triggers.erase(it);
                changed = true;
            } else {
                ++it;  // e.g. the clock is not set yet: tried again later
            }
        }
        if (changed) save_triggers();
    }
    for (const auto& m : missed) note_recent(m);
}

bool enqueue(const Item& item) {
    std::lock_guard<std::mutex> guard(lock);
    if (queue.size() >= kMaxQueue) {
        runtime_event("prog_queue_full", item.label);
        for (auto& t : triggers) {
            if (t.id == item.trigger) t.queued = false;  // dropped: the next tick may queue it again
        }
        return false;
    }
    queue.push_back(item);
    return true;
}

// "prog://名字?参数" -> name, query
void split_run(const String& run, String& name, String& query) {
    String rest = run.substring(7);
    const int q = rest.indexOf('?');
    query = q < 0 ? String() : rest.substring(q + 1);
    name = url_decode(q < 0 ? rest : rest.substring(0, q));
}

String item_json(const Item& item) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", item.label.c_str());
    cJSON_AddNumberToObject(o, "trigger", item.trigger);
    cJSON_AddStringToObject(o, "program", item.program.c_str());
    cJSON_AddStringToObject(o, "query", item.query.c_str());
    cJSON_AddStringToObject(o, "data", item.data.c_str());
    const String out = json_print(o);
    cJSON_Delete(o);
    return out;
}

// A trigger's run ended: its status; kFailuresToPause failures in a row pause it.
void trigger_done(int id, bool ok, const String& note) {
    String paused;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto& t : triggers) {
            if (t.id != id) continue;
            t.queued = false;
            t.last_run = time(nullptr);
            t.last_ok = ok;
            String line = note;
            const int nl = line.indexOf('\n');
            if (nl >= 0) line = line.substring(0, nl);
            t.last_note = utf8_head(line, 80);
            ++t.runs;
            t.failures = ok ? 0 : t.failures + 1;
            if (t.failures >= kFailuresToPause && !t.paused) {
                t.paused = true;
                t.armed = false;
                t.paused_why = "连续失败 " + String(t.failures) + " 次：" + t.last_note;
                paused = "触发 #" + String(id) + " 连续失败 " + String(t.failures) + " 次，已自动停用";
                save_triggers();
            }
        }
    }
    if (!paused.isEmpty()) {
        note_recent(paused);
        ui::set_notice(paused, true);
    }
}

// One queued run, on the worker task.
void job(uint32_t job_id, const String& text) {
    cJSON* o = cJSON_Parse(text.c_str());
    const String label = json_str(o, "label");
    const String program = json_str(o, "program");
    const String query = json_str(o, "query");
    const String data = json_str(o, "data");
    const int trigger = json_int(o, "trigger", 0);
    cJSON_Delete(o);
    ui::set_busy("RUN 程序");
    ui::entry_begin("[程序] " + label);
    const Outcome r = run_program(program, query, data, false);
    const String summary = r.ok ? (r.result.isEmpty() ? String("完成") : r.result) : r.error;
    ui::entry_finish(r.ok ? ui::Status::Done : ui::Status::Failed, summary, r.output, "[]");
    worker::set_result(job_id, r.ok ? r.result : String(), r.ok ? String() : r.error);
    ui::set_idle();
    if (trigger) trigger_done(trigger, r.ok, summary);
}

String run_text(const String& name, const Outcome& r) {
    String out = r.ok ? "程序「" + name + "」完成，结果：" + (r.result.isEmpty() ? String("（无）") : r.result)
                      : "ERROR: 程序「" + name + "」失败：" + r.error;
    if (!r.output.isEmpty()) out += "\n输出：\n" + r.output;
    return out;
}

}  // namespace

// ---------------------------------------------------------------- events and the loop

void on_event(const String& target, const String& data) {
    std::lock_guard<std::mutex> guard(lock);
    if (events_in.size() < 16) events_in.push_back({target, data});
}

void loop() {
    if (!loaded) {
        if (!storage::ready()) return;
        loaded = true;
        if (!storage::exists(kIndex)) scan();
        load_triggers();
        last_arm = millis() - 20000;  // arm right away
    }
    if (millis() - last_arm > 10000) {  // (again, if the clock was not set)
        last_arm = millis();
        arm_triggers();
    }
    std::vector<std::pair<String, String>> incoming;
    {
        std::lock_guard<std::mutex> guard(lock);
        incoming.swap(events_in);
    }
    for (const auto& event : incoming) {
        std::vector<Item> due;
        {
            std::lock_guard<std::mutex> guard(lock);
            bool changed = false;
            for (auto it = triggers.begin(); it != triggers.end();) {
                if (it->paused || it->when != event.first) {
                    ++it;
                    continue;
                }
                Item item;
                item.label = "触发 #" + String(it->id);
                item.trigger = it->id;
                split_run(it->run, item.program, item.query);
                item.data = event.second;
                it->queued = true;
                due.push_back(item);
                if (one_off(it->when)) {
                    it = triggers.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
            if (changed) save_triggers();
        }
        for (const auto& item : due) enqueue(item);
    }

    // Interval triggers: due now, unless the last run is still queued or the
    // device is busy, in which case this tick is skipped (runs never pile up).
    {
        std::vector<Item> due;
        const bool busy = worker::busy() || agent::active();
        {
            std::lock_guard<std::mutex> guard(lock);
            const uint32_t now = millis();
            for (auto& t : triggers) {
                const long every = interval_of(t.when);
                if (every <= 0 || t.paused || !t.armed || static_cast<int32_t>(now - t.next_ms) < 0) continue;
                t.next_ms = now + every * 1000UL;
                if (t.queued || busy) {
                    ++t.skipped;
                    continue;
                }
                Item item;
                item.label = "触发 #" + String(t.id);
                item.trigger = t.id;
                split_run(t.run, item.program, item.query);
                t.queued = true;
                due.push_back(item);
            }
        }
        for (const auto& item : due) enqueue(item);
    }

    if (worker::busy() || agent::active()) return;  // never while a conversation is open
    Item next;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (queue.empty()) return;
        next = queue.front();
    }
    worker::Job spec;
    spec.kind = worker::Kind::Callback;
    spec.callback = job;
    spec.text = item_json(next);
    if (worker::submit(spec)) {
        std::lock_guard<std::mutex> guard(lock);
        if (!queue.empty()) queue.erase(queue.begin());
    }
}

// ---------------------------------------------------------------- prog://

String list_text() {
    String out;
    int n = 0;
    for (const auto& e : entries()) {
        if (!e.header.package.isEmpty()) continue;
        ++n;
        out += "- " + signature(e.name, e.header);
        if (!e.header.about.isEmpty()) out += "：" + e.header.about;
        if (e.state != State::Enabled) out += String("（") + state_word(e.state) + "）";
        const String used = users_of(e.name);
        if (!used.isEmpty()) out += "；被" + used + "使用";
        out += "\n";
    }
    String head = n ? "共 " + String(n) + " 个程序：\n" : String("还没有程序。写一个：write prog://名字，content 为 Lua 源码（read prog://help）\n");
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!recent.empty()) {
            out += "最近：\n";
            for (const auto& r : recent) out += "  " + r + "\n";
        }
    }
    const String legacy = legacy_text();
    if (!legacy.startsWith("没有")) out += legacy + "\n";
    out = head + out;
    out.trim();
    return out;
}

String show(const String& name) {
    Entry e;
    if (!load_entry(name, e)) return "ERROR: 没有程序「" + name + "」（read prog://list 查看）";
    String out = signature(name, e.header) + String("（") + state_word(e.state) + "，" + String(static_cast<unsigned>(e.size)) +
                 " 字节）";
    if (!e.problem.isEmpty()) out += "\n声明有误：" + e.problem;
    for (const auto& u : e.header.uses) out += "\n权限：" + lua_meta::describe(u);
    return out + "\n源码：\n" + storage::read_text(path_of(name), lua_meta::kMaxSource);
}

String save(const String& name, const String& source, bool approved) {
    String err = lua_meta::check_name(name);
    lua_meta::Header h;
    if (err.isEmpty()) err = lua_meta::parse(source, h);
    if (err.isEmpty() && !h.package.isEmpty()) err = "这是扩展包（@package），放进卡里后用 pkg:// 管理";
    String trimmed = source;
    trimmed.trim();
    if (err.isEmpty() && trimmed.startsWith("{")) err = "程序现在用 Lua 写（read prog://help）；旧的 JSON 格式不再支持";
    if (err.isEmpty()) {
        const String problem = luavm::compile(source, name);
        if (!problem.isEmpty()) err = "语法错误：" + problem;
    }
    const bool existed = storage::exists(path_of(name));
    if (err.isEmpty() && !existed && index_names().size() >= kMaxFiles) err = "程序已达上限 " + String(kMaxFiles) + " 个，先删掉不用的";
    if (err.isEmpty() && must_confirm(h) && !approved) err = "声明了需要确认的能力，必须由用户确认后保存";
    if (!err.isEmpty()) return "ERROR: " + err;
    if (!storage::ensure_parent(path_of(name)) || !storage::write_text(path_of(name), source)) return "ERROR: 写卡失败";
    if (must_confirm(h)) confirm(name, h);
    else forget(kApproved, name);
    index_add(name);
    if (!existed) record_stat(name, true, true, "");
    String out = (existed ? "已修改程序「" : "已保存程序「") + signature(name, h) + "」";
    if (!h.uses.empty()) out += "，已确认 " + String(static_cast<int>(h.uses.size())) + " 项权限";
    return out + "。运行：exec prog://" + name + "；定时或开机运行：write trigger://new";
}

String remove(const String& name) {
    if (!storage::exists(path_of(name))) return "ERROR: 没有程序「" + name + "」";
    const String used = users_of(name);
    if (!used.isEmpty()) return "ERROR: 「" + name + "」还被" + used + "使用，先删掉或改掉它们";
    storage::remove(path_of(name));
    storage::remove(old_path_of(name));
    forget(kApproved, name);
    forget(kStats, name);
    index_remove(name);
    return "已删除「" + name + "」";
}

String run(const String& name, const String& query, const String& stdin_text) {
    return run_text(name, run_program(name, query, "", true, stdin_text));
}

bool launch(const String& name) {
    worker::Job job;
    job.kind = worker::Kind::Callback;
    job.text = name;
    job.callback = [](uint32_t id, const String& program) {
        ask_inputs = true;
        const String out = run(program, "");
        ask_inputs = false;
        worker::set_result(id, out);
    };
    return worker::submit(job) != 0;
}

std::vector<std::pair<String, String>> runnable() {
    std::vector<std::pair<String, String>> out;
    for (const auto& e : entries()) {
        if (e.header.package.isEmpty()) out.push_back({e.name, e.header.about});
    }
    return out;
}

bool needs_confirm(const String& source) {
    lua_meta::Header h;
    return lua_meta::parse(source, h).isEmpty() && must_confirm(h);
}

String save_summary(const String& name, const String& source) {
    lua_meta::Header h;
    lua_meta::parse(source, h);
    String out = "保存程序「" + name + "」" + (h.about.isEmpty() ? String() : "（" + h.about + "）") + "，允许它：";
    for (std::size_t i = 0; i < h.uses.size(); ++i) out += (i ? "；" : "") + lua_meta::describe(h.uses[i]);
    return out;
}

String legacy_text() {
    cJSON* list = cJSON_Parse(storage::list_dir(kLegacyDir).c_str());
    String names;
    int n = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (json_bool(item, "dir", false)) continue;
        cJSON* p = read_json(String(kLegacyDir) + "/" + json_str(item, "name"), 16384);
        const String name = json_str(p, "name");
        cJSON_Delete(p);
        if (name.isEmpty()) continue;
        names += String(n++ ? "、" : "") + name;
    }
    cJSON_Delete(list);
    if (!n) return "没有旧格式的程序";
    return "旧格式（JSON）程序 " + String(n) + " 个，已不能运行：" + names +
           "。翻译成 Lua：read prog://legacy/名字 看原文，再 write prog://名字";
}

String legacy_show(const String& name) {
    cJSON* list = cJSON_Parse(storage::list_dir(kLegacyDir).c_str());
    String out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        cJSON* p = read_json(String(kLegacyDir) + "/" + json_str(item, "name"), 16384);
        if (json_str(p, "name") == name) out = json_print(p);
        cJSON_Delete(p);
        if (!out.isEmpty()) break;
    }
    cJSON_Delete(list);
    return out.isEmpty() ? "ERROR: 没有旧程序「" + name + "」" : "旧程序「" + name + "」原文（JSON，请按 read prog://help 翻译成 Lua）：\n" + out;
}

// ---------------------------------------------------------------- trigger://

String trigger_line(int id) {
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& t : triggers) {
        if (t.id != id) continue;
        String name, query;
        split_run(t.run, name, query);
        String line = "#" + String(t.id) + " " + describe_when(t.when) + " → 运行 " + name +
                      (query.isEmpty() ? String() : "（" + url_decode(query) + "）") +
                      (t.paused ? (t.paused_why.isEmpty() ? String("（已停用）") : "（已自动停用：" + t.paused_why + "）")
                                : t.armed ? String() : String("（等待对时）"));
        if (t.runs) {
            line += "；开机以来运行 " + String(t.runs) + " 次，上次 " + clock_text(t.last_run, "%H:%M:%S") +
                    (t.last_ok ? " 成功" : " 失败：" + t.last_note);
            if (t.failures) line += "，连续失败 " + String(t.failures) + " 次";
        }
        if (t.skipped) line += "；忙时跳过 " + String(t.skipped) + " 次";
        return line;
    }
    return "";
}

String triggers_text() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& t : triggers) ids.push_back(t.id);
    }
    if (ids.empty()) return "还没有触发。新建：write trigger://new，content 如 {\"when\":\"time://every/07:00\",\"run\":\"prog://早安\"}";
    String out = "共 " + String(ids.size()) + " 个触发：\n";
    for (const int id : ids) out += trigger_line(id) + "\n";
    out.trim();
    return out;
}

String add_trigger(const String& content) {
    cJSON* t = cJSON_Parse(content.c_str());
    String when = json_str(t, "when");
    String run = json_str(t, "run");
    if (run.isEmpty()) run = json_str(json_obj(t, "run"), "exec");  // the old {"exec": "prog://…"} form
    cJSON_Delete(t);
    String err;
    if (when.isEmpty()) err = "缺少 when（如 time://every/07:00、time://in/10m、device://boot）";
    else if (!run.startsWith("prog://")) err = "run 要写成 prog://程序名?参数（触发只能运行程序）";
    String name, query;
    if (err.isEmpty()) {
        split_run(run, name, query);
        Entry e;
        if (!load_entry(name, e)) err = "没有程序「" + name + "」，先写好它再建触发";
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        if (err.isEmpty() && triggers.size() >= kMaxTriggers) err = "触发已达上限 " + String(kMaxTriggers) + " 个";
    }
    String normalized;
    const long every = interval_of(when);
    if (err.isEmpty() && every < 0) err = "时间间隔写成 time://every/30s、every/5m、every/2h（每天某时刻写 every/07:00）";
    else if (err.isEmpty() && every > 0 && every < kMinInterval) err = "间隔至少 " + String(kMinInterval) + " 秒";
    else if (err.isEmpty() && every > 0) normalized = when;
    else if (err.isEmpty()) err = registry::watch(when, normalized);
    if (!err.isEmpty()) return "ERROR: " + err;
    Trigger trig;
    trig.when = normalized;
    trig.run = run;
    trig.armed = true;
    if (every > 0) trig.next_ms = millis() + every * 1000UL;
    {
        std::lock_guard<std::mutex> guard(lock);
        int id = 1;
        for (const auto& x : triggers) id = std::max(id, x.id + 1);
        trig.id = id;
        triggers.push_back(trig);
        save_triggers();
    }
    return "已新建触发 " + trigger_line(trig.id);
}

String trigger_action(int id, const String& action, bool approved) {
    String when;
    bool shared = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        Trigger* found = nullptr;
        for (auto& t : triggers) {
            if (t.id == id) found = &t;
        }
        if (!found) return "ERROR: 没有触发 #" + String(id) + "（read trigger://list 查看）";
        when = found->when;
        for (const auto& t : triggers) shared = shared || (t.id != id && t.when == when && !t.paused);
        if (action == "pause") {
            found->paused = true;
            found->armed = false;
        } else if (action == "resume") {
            found->paused = false;
            found->paused_why = String();
            found->failures = 0;
            found->armed = false;  // armed again below
        } else if (action == "delete") {
            if (!approved) return "ERROR: 删除触发需要用户确认";
            triggers.erase(std::remove_if(triggers.begin(), triggers.end(), [&](const Trigger& t) { return t.id == id; }),
                           triggers.end());
        } else {
            return "ERROR: 触发只能 pause、resume、delete";
        }
        save_triggers();
    }
    if (action != "resume" && !shared && interval_of(when) == 0) registry::unwatch(when);
    if (action == "resume") arm_triggers();
    if (action == "delete") return "已删除触发 #" + String(id);
    return trigger_line(id);
}

// What clean() would remove: triggers whose program is gone, and those that
// paused themselves after failing.  "" when there is nothing.
String clean_plan(std::vector<int>* ids) {
    std::vector<std::pair<int, String>> list;  // id, run
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& t : triggers) {
            if (!t.paused_why.isEmpty()) list.push_back({t.id, String()});
            else list.push_back({t.id, t.run});
        }
    }
    String out;
    for (const auto& item : list) {
        String why;
        if (item.second.isEmpty()) {
            why = "失败后已自动停用";
        } else {
            String name, query;
            split_run(item.second, name, query);
            Entry e;
            if (!load_entry(name, e)) why = "程序「" + name + "」已经不在了";
        }
        if (why.isEmpty()) continue;
        if (ids) ids->push_back(item.first);
        out += String(out.isEmpty() ? "" : "；") + "#" + String(item.first) + "（" + why + "）";
    }
    return out;
}

String trigger_clean_summary() {
    const String plan = clean_plan(nullptr);
    return plan.isEmpty() ? String("清理触发：没有要清理的") : "清理触发，删除 " + plan;
}

String trigger_clean(bool approved) {
    std::vector<int> ids;
    const String plan = clean_plan(&ids);
    if (ids.empty()) return "没有要清理的触发（程序都在，也没有失败后自动停用的）";
    if (!approved) return "ERROR: 清理触发需要用户确认";
    std::vector<String> whens;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const int id : ids) {
            for (auto it = triggers.begin(); it != triggers.end(); ++it) {
                if (it->id != id) continue;
                whens.push_back(it->when);
                triggers.erase(it);
                break;
            }
        }
        save_triggers();
    }
    for (const auto& when : whens) {
        bool shared = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (const auto& t : triggers) shared = shared || t.when == when;
        }
        if (!shared && interval_of(when) == 0) registry::unwatch(when);
    }
    return "已清理 " + String(static_cast<int>(ids.size())) + " 个触发：" + plan;
}

// ---------------------------------------------------------------- pkg://

String scan() {
    cJSON* list = cJSON_Parse(storage::list_dir(kDir).c_str());
    std::vector<String> names;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        const String file = json_str(item, "name");
        if (json_bool(item, "dir", false) || !file.endsWith(".lua")) continue;
        const String name = file.substring(0, file.length() - 4);
        if (lua_meta::check_name(name).isEmpty() && names.size() < kMaxFiles) names.push_back(name);
    }
    cJSON_Delete(list);
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) { return std::strcmp(a.c_str(), b.c_str()) < 0; });
    write_index(names);
    // A confirmation outlives its file otherwise: a new file of the same name
    // (and the same uses) would come back enabled without the user seeing it.
    cJSON* approved = object_at(kApproved);
    std::vector<String> stale;
    cJSON_ArrayForEach(item, approved) {
        if (std::find(names.begin(), names.end(), String(item->string)) == names.end()) stale.push_back(item->string);
    }
    for (const auto& name : stale) cJSON_DeleteItemFromObjectCaseSensitive(approved, name.c_str());
    if (!stale.empty()) write_json(kApproved, approved);
    cJSON_Delete(approved);
    return "扫描完成：" + String(static_cast<int>(names.size())) + " 个 .lua 文件\n" + pkg_list();
}

String pkg_list() {
    const auto all = entries();
    if (all.empty()) return "卡上还没有程序或扩展包（" + String(kDir) + "）";
    String out;
    for (const auto& e : all) {
        out += "- " + e.name + (e.header.package.isEmpty() ? String(" [程序]") : " [扩展包 " + e.header.target + "]");
        if (!e.header.version.isEmpty()) out += " v" + e.header.version;
        out += String(" ") + state_word(e.state);
        if (!e.header.about.isEmpty()) out += "：" + e.header.about;
        if (!e.problem.isEmpty()) out += "（" + e.problem + "）";
        if (!e.header.uses.empty()) {
            out += "；权限：";
            for (std::size_t i = 0; i < e.header.uses.size(); ++i) out += (i ? "，" : "") + lua_meta::describe(e.header.uses[i]);
        }
        out += "\n";
    }
    out.trim();
    return out;
}

bool pkg_needs_confirm(const String& name, const String& action) {
    if (action == "delete") return true;
    if (action != "enable") return false;
    Entry e;
    return load_entry(name, e) && must_confirm(e.header);
}

String pkg_summary(const String& name, const String& action) {
    Entry e;
    if (!load_entry(name, e)) return {};
    if (action == "delete") return "删除「" + name + "」（" + path_of(name) + "）";
    String out = "启用「" + name + "」" + (e.header.about.isEmpty() ? String() : "（" + e.header.about + "）");
    if (!e.header.package.isEmpty()) {
        out += "，扩展包，负责 " + e.header.target + "；告诉肥鱼：" + utf8_head(e.header.usage, 200);
        if (e.header.uses.empty()) return out;
    }
    out += "，允许它：";
    for (std::size_t i = 0; i < e.header.uses.size(); ++i) out += (i ? "；" : "") + lua_meta::describe(e.header.uses[i]);
    return out;
}

std::vector<PackageInfo> card_packages() {
    std::vector<PackageInfo> out;
    for (const auto& e : entries()) {
        if (e.header.package.isEmpty() || e.state != State::Enabled) continue;
        out.push_back({e.name, e.header.target, e.header.usage, e.header.keywords, e.header.about});
    }
    return out;
}

uint32_t generation() { return changes.load(); }

bool installed(const String& name, lua_meta::Header& header) {
    Entry e;
    if (!load_entry(name, e)) return false;
    header = e.header;
    return true;
}

bool has_previous(const String& name) { return storage::exists(old_path_of(name)); }

String adopt(const String& name, const String& file, const lua_meta::Header& header, bool enable) {
    const String path = path_of(name);
    const String old = old_path_of(name);
    const bool had = storage::exists(path);
    if (had && !(storage::ensure_parent(old) && storage::rename(path, old))) return "旧版本没能保留，未安装";
    if (!storage::rename(file, path)) {
        if (had) storage::rename(old, path);
        return "写入卡上失败";
    }
    if (enable) confirm(name, header);
    else forget(kApproved, name);  // a file from an address: the user enables it after reading it
    index_add(name);
    return {};
}

String rollback(const String& name) {
    const String path = path_of(name);
    const String old = old_path_of(name);
    if (!storage::exists(old)) return "ERROR: 「" + name + "」没有保留的上一版";
    const String swap = String(kDir) + "/old/" + name + ".swap";
    if (!storage::rename(path, swap) || !storage::rename(old, path) || !storage::rename(swap, old)) {
        return "ERROR: 退回失败（卡上文件可能要 write pkg://scan 后检查）";
    }
    index_add(name);
    Entry e;
    load_entry(name, e);
    return "已退回「" + name + "」" + (e.header.version.isEmpty() ? String() : " v" + e.header.version) + "，状态：" +
           state_word(e.state) + (e.state == State::Pending ? "（权限和当前确认的不同，write pkg://" + name + "/enable）" : String());
}

String run_package(const String& name, const String& tool, const cJSON* arguments) {
    Entry e;
    if (!load_entry(name, e) || e.header.package.isEmpty()) {
        return "ERROR: 卡上没有扩展包「" + name + "」（write pkg://scan 重新扫描）";
    }
    if (e.state != State::Enabled) {
        return "ERROR: 扩展包「" + name + "」" + state_word(e.state) + "（write pkg://" + name + "/enable）";
    }
    luavm::Options options;
    options.uses = &e.header.uses;
    options.input = arguments;
    options.entry = tool.c_str();
    String output, result, error;
    luavm::Stats stats;
    const bool ok = luavm::run(storage::read_text(path_of(name), lua_meta::kMaxSource), name, options, output, result,
                               error, stats);
    runtime_event("pkg_run", name + " " + tool + (ok ? " done" : " failed") + " peak=" +
                                 String(static_cast<unsigned>(stats.peak)) + " ms=" + String(stats.ms));
    if (!ok) return "ERROR: " + error + (output.isEmpty() ? String() : "\n输出：" + output);
    if (result.isEmpty()) return output.isEmpty() ? String("完成") : output;
    return output.isEmpty() ? result : result + "\n输出：" + output;
}

String pkg_action(const String& name, const String& action, bool approved) {
    Entry e;
    if (!load_entry(name, e)) return "ERROR: 卡上没有「" + name + "」（write pkg://scan 重新扫描）";
    if (action == "delete") {
        if (!approved) return "ERROR: 删除需要用户确认";
        return remove(name);
    }
    if (action == "disable") {
        set_off(name, true);
        return "已停用「" + name + "」";
    }
    if (action == "enable") {
        if (e.state == State::Broken) return "ERROR: 声明有误，先修正：" + e.problem;
        if (must_confirm(e.header)) {
            if (!approved) return "ERROR: 启用需要用户确认它的权限";
            confirm(name, e.header);
        } else {
            forget(kApproved, name);
        }
        index_add(name);
        return "已启用「" + name + "」";
    }
    return "ERROR: pkg://名字/ 后面只能是 enable、disable 或 delete";
}

}  // namespace vat::programs
