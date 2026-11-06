#include "prog_runtime.h"

#include <time.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "agent.h"
#include "common.h"
#include "policy.h"
#include "prog_engine.h"
#include "registry.h"
#include "storage.h"
#include "ui_state.h"
#include "worker.h"

namespace vat::programs {
namespace {

constexpr char kProgramDir[] = "/vat/things/prog/programs";
constexpr char kTriggers[] = "/vat/things/prog/triggers.json";
constexpr char kApproved[] = "/vat/things/prog/approved.json";
constexpr char kStats[] = "/vat/prog/stats.json";
constexpr char kRunDir[] = "/vat/prog/runs";
constexpr std::size_t kMaxPrograms = 64;
constexpr std::size_t kMaxTriggers = 32;
constexpr std::size_t kMaxQueue = 8;
constexpr uint32_t kSliceMs = 90000;  // the worker watchdog restarts the device at 180 s
constexpr long kUnusedSeconds = 30L * 86400;
constexpr char kTriggerProgram[] = "~trigger";  // synthetic program wrapping a trigger's step (names starting with ~ are reserved)

struct Trigger {
    int id = 0;
    String when;
    String run;       // one step, JSON
    String approved;  // fingerprints, JSON array
    bool paused = false;
    bool armed = false;
};

struct Item {  // a queued run
    String kind;  // trigger | run | resume
    String a;     // trigger: label · run: program name · resume: run id
    String b;     // trigger: step JSON · run: query
    String c;     // trigger: approvals JSON
    String data;  // event data
};

std::vector<Trigger> triggers;
std::vector<Item> queue;
std::vector<std::pair<String, String>> events_in;  // target, data
std::vector<String> recent;                        // last few results, newest last
std::mutex lock;
bool loaded = false;
uint32_t last_arm = 0;
int next_run = 1;

// ---------------------------------------------------------------- small helpers

String hex8(uint32_t v) {
    char text[9];
    snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(v));
    return text;
}

uint32_t name_hash(const String& name) {
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < name.length(); ++i) {
        h ^= static_cast<uint8_t>(name[i]);
        h *= 16777619u;
    }
    return h;
}

// Program files are named by a hash of the program name (the name itself may
// hold any characters); the name lives inside the file.
String program_path(const String& name) { return String(kProgramDir) + "/" + hex8(name_hash(name)) + ".json"; }

String clock_text(time_t t, const char* format) {
    if (t < 1700000000) return "--";
    struct tm tm;
    localtime_r(&t, &tm);
    char text[24];
    strftime(text, sizeof(text), format, &tm);
    return text;
}

bool failed_text(const String& out) { return out.startsWith("ERROR") || out.startsWith("没有能力包可处理"); }

cJSON* read_json(const char* path, std::size_t max = 32768) {
    return cJSON_Parse(storage::read_text(path, max).c_str());
}

bool write_json(const String& path, const cJSON* value) {
    char* printed = cJSON_Print(value);
    if (!printed) return false;
    const bool ok = storage::ensure_parent(path) && storage::write_text(path, String(printed));
    cJSON_free(printed);
    return ok;
}

cJSON* load_program(const String& name) {
    cJSON* p = cJSON_Parse(storage::read_text(program_path(name), 16384).c_str());
    if (p && json_str(p, "name") != name) {  // hash collision or a stale file
        cJSON_Delete(p);
        return nullptr;
    }
    return p;
}

// Every stored program, parsed (caller deletes each).
std::vector<cJSON*> all_programs() {
    std::vector<cJSON*> out;
    cJSON* list = cJSON_Parse(storage::list_dir(kProgramDir).c_str());
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (json_bool(item, "dir", false)) continue;
        const String file = String(kProgramDir) + "/" + json_str(item, "name");
        if (cJSON* p = cJSON_Parse(storage::read_text(file, 16384).c_str())) out.push_back(p);
    }
    cJSON_Delete(list);
    return out;
}

void free_all(std::vector<cJSON*>& list) {
    for (cJSON* p : list) cJSON_Delete(p);
    list.clear();
}

void note_recent(const String& line) {
    std::lock_guard<std::mutex> guard(lock);
    recent.push_back(clock_text(time(nullptr), "%H:%M") + " " + utf8_head(line, 120));
    if (recent.size() > 6) recent.erase(recent.begin());
}

// ---------------------------------------------------------------- approvals and stats

bool approved_in(const cJSON* list, uint32_t fingerprint) {
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (cJSON_IsNumber(item) && static_cast<uint32_t>(item->valuedouble) == fingerprint) return true;
    }
    return false;
}

void set_approvals(const String& name, const std::vector<uint32_t>& prints) {
    cJSON* root = read_json(kApproved);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, name.c_str());
    if (!prints.empty()) {
        cJSON* list = cJSON_AddArrayToObject(root, name.c_str());
        for (const uint32_t p : prints) cJSON_AddItemToArray(list, cJSON_CreateNumber(p));
    }
    write_json(kApproved, root);
    cJSON_Delete(root);
}

cJSON* stats_root() {
    cJSON* root = read_json(kStats);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

void record_stat(const String& name, bool created, bool ok, const String& result) {
    cJSON* root = stats_root();
    cJSON* entry = json_obj(root, name.c_str());
    if (!entry) entry = cJSON_AddObjectToObject(root, name.c_str());
    auto put = [&](const char* key, cJSON* value) {
        cJSON_DeleteItemFromObjectCaseSensitive(entry, key);
        cJSON_AddItemToObject(entry, key, value);
    };
    const double now = static_cast<double>(time(nullptr));
    if (created) {
        put("created", cJSON_CreateNumber(now));
    } else {
        put("last", cJSON_CreateNumber(now));
        put("ok", cJSON_CreateBool(ok));
        put("result", cJSON_CreateString(utf8_head(result, 80).c_str()));
    }
    write_json(kStats, root);
    cJSON_Delete(root);
}

void forget_stat(const String& name) {
    cJSON* root = stats_root();
    cJSON_DeleteItemFromObjectCaseSensitive(root, name.c_str());
    write_json(kStats, root);
    cJSON_Delete(root);
}

// ---------------------------------------------------------------- descriptions

String describe_when(const String& when) {
    const int slash = when.indexOf('/', 7);
    const String kind = when.startsWith("time://") && slash > 0 ? when.substring(7, slash) : String();
    const String value = slash > 0 ? when.substring(slash + 1) : String();
    if (kind == "every") return "每天 " + value;
    if (kind == "weekday") return "工作日 " + value;
    if (kind == "weekend") return "周末 " + value;
    if (kind == "at") return value.substring(5, 16) + " 一次";
    if (when == "device://boot") return "每次开机";
    return when;
}

String describe_args(const String& tool, const cJSON* args) {
    String line = tool + " " + json_str(args, "target");
    if (tool == "exec") {
        const String program = json_str(args, "program");
        if (!program.isEmpty()) line += " $ " + program;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(args, "args")) {
            if (cJSON_IsString(item)) line += String(" ") + item->valuestring;
        }
    }
    const String content = json_str(args, "content");
    if (!content.isEmpty()) line += "：" + utf8_head(content, 40);
    return utf8_head(line, 120);
}

String describe_step(const cJSON* step) {
    String tool;
    cJSON* args = prog::action_arguments(step, tool);
    String name, query;
    String line = prog::program_target(json_str(args, "target"), name, query)
                      ? "运行 " + name + (query.isEmpty() ? String() : "（" + url_decode(query) + "）")
                      : describe_args(tool, args);
    cJSON_Delete(args);
    return line;
}

// Steps Policy would ask about (fingerprints + summaries); an error when a
// step would be refused outright.
String sensitive_steps(const cJSON* steps, std::vector<uint32_t>& prints, std::vector<String>& lines) {
    String error;
    prog::for_each_action(steps, [&](const String&, const cJSON* step) {
        String tool;
        cJSON* args = prog::action_arguments(step, tool);
        const auto verdict = policy::evaluate(tool, args);
        if (verdict.decision == policy::Decision::Deny && error.isEmpty()) {
            error = "「" + describe_args(tool, args) + "」会被拒绝：" + verdict.reason;
        } else if (verdict.decision == policy::Decision::Ask) {
            prints.push_back(prog::fingerprint(step));
            lines.push_back(describe_args(tool, args));
        } else if (!registry::needs_secret(tool, args).isEmpty() && error.isEmpty()) {
            error = "「" + describe_args(tool, args) + "」需要输入密码，程序里不能用";
        }
        cJSON_Delete(args);
    });
    return error;
}

String outline(const cJSON* steps, const String& prefix, int indent) {
    String out;
    int n = 0;
    const cJSON* step = nullptr;
    cJSON_ArrayForEach(step, steps) {
        const String number = prefix + String(++n);
        String pad;
        for (int i = 0; i < indent; ++i) pad += "  ";
        if (cJSON_GetObjectItemCaseSensitive(step, "if")) {
            String cond = json_str(step, "if");
            for (const char* key : {"is", "not", "has", "above", "below"}) {
                const cJSON* v = cJSON_GetObjectItemCaseSensitive(step, key);
                if (!v) continue;
                const char* word = !strcmp(key, "is") ? " 是 " : !strcmp(key, "not") ? " 不是 " : !strcmp(key, "has") ? " 包含 "
                                   : !strcmp(key, "above") ? " 大于 " : " 小于 ";
                cond += word + (cJSON_IsString(v) ? String(v->valuestring) : String(v->valuedouble, 0));
            }
            out += pad + number + " 如果 " + cond + "：\n";
            out += outline(cJSON_GetObjectItemCaseSensitive(step, "then"), number + ".", indent + 1);
            const cJSON* otherwise = cJSON_GetObjectItemCaseSensitive(step, "else");
            if (cJSON_GetArraySize(otherwise) > 0) {
                out += pad + "   否则：\n" + outline(otherwise, number + ".", indent + 1);
            }
        } else if (cJSON_GetObjectItemCaseSensitive(step, "repeat")) {
            out += pad + number + " 重复 " + String(json_int(step, "repeat", 0)) + " 次：\n";
            out += outline(cJSON_GetObjectItemCaseSensitive(step, "steps"), number + ".", indent + 1);
        } else if (cJSON_GetObjectItemCaseSensitive(step, "wait")) {
            out += pad + number + " 等待 " + json_str(step, "wait") + "\n";
        } else {
            const String as = json_str(step, "as");
            out += pad + number + " " + describe_step(step) + (as.isEmpty() ? String() : " → " + as) +
                   (json_str(step, "on_error") == "continue" ? String("（失败继续）") : String()) + "\n";
        }
    }
    return out;
}

String signature(const prog::Info& info) {
    if (info.inputs.empty()) return info.name;
    String sig = info.name + "(";
    for (std::size_t i = 0; i < info.inputs.size(); ++i) {
        if (i) sig += ", ";
        sig += info.inputs[i].name;
        if (!info.inputs[i].required) sig += "=" + info.inputs[i].fallback;
    }
    return sig + ")";
}

// ---------------------------------------------------------------- triggers on storage

void save_triggers() {  // caller holds `lock`
    cJSON* list = cJSON_CreateArray();
    for (const auto& t : triggers) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", t.id);
        cJSON_AddStringToObject(item, "when", t.when.c_str());
        cJSON_AddItemToObject(item, "run", cJSON_Parse(t.run.c_str()));
        cJSON* approvals = cJSON_Parse(t.approved.c_str());
        cJSON_AddItemToObject(item, "approved", approvals ? approvals : cJSON_CreateArray());
        cJSON_AddBoolToObject(item, "paused", t.paused);
        cJSON_AddItemToArray(list, item);
    }
    write_json(kTriggers, list);
    cJSON_Delete(list);
}

void load_triggers() {
    std::lock_guard<std::mutex> guard(lock);
    triggers.clear();
    cJSON* list = read_json(kTriggers);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        Trigger t;
        t.id = json_int(item, "id", 0);
        t.when = json_str(item, "when");
        t.run = json_print(json_obj(item, "run"));
        const cJSON* approvals = cJSON_GetObjectItemCaseSensitive(item, "approved");
        t.approved = approvals ? json_print(approvals) : String("[]");
        t.paused = json_bool(item, "paused", false);
        if (t.id > 0 && !t.when.isEmpty()) triggers.push_back(t);
    }
    cJSON_Delete(list);
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

// ---------------------------------------------------------------- waiting runs

String run_path(int id) { return String(kRunDir) + "/" + String(id) + ".json"; }

String resume_target(time_t when, int id) {
    return "time://at/" + clock_text(when, "%Y-%m-%d %H:%M:%S") + "#r" + String(id);
}

void arm_runs() {
    cJSON* list = cJSON_Parse(storage::list_dir(kRunDir).c_str());
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        const String file = json_str(item, "name");
        const int id = file.toInt();
        if (id <= 0) continue;
        next_run = std::max(next_run, id + 1);
        cJSON* run = read_json(run_path(id).c_str(), 16384);
        const String target = json_str(run, "resume");
        const String label = json_str(run, "label");
        cJSON_Delete(run);
        String normalized;
        const String err = target.isEmpty() ? String("no resume") : registry::watch(target, normalized);
        if (err.indexOf("已经过去") >= 0 || target.isEmpty()) {
            storage::remove(run_path(id));
            note_recent("放弃「" + label + "」：等待期间关机，已过点");
        }
        // Any other error (clock not set): watched again on the next pass.
    }
    cJSON_Delete(list);
}

// ---------------------------------------------------------------- the device Host

class DeviceHost : public prog::Host {
public:
    explicit DeviceHost(bool slice) : slice_(slice), started_(millis()) {}
    String trigger_json;      // the synthetic program of a trigger run
    String trigger_approved;  // its approvals

    bool call(const String& program, const String& tool, const cJSON* arguments, const cJSON* step,
              String& out) override {
        const auto verdict = policy::evaluate(tool, arguments);
        if (verdict.decision == policy::Decision::Deny) {
            out = "ERROR: 已拒绝：" + verdict.reason;
            return false;
        }
        bool approved = false;
        if (verdict.decision == policy::Decision::Ask) {
            if (!is_approved(program, prog::fingerprint(step))) {
                out = "ERROR: 这一步需要确认，但保存时没有确认过（程序被改过？重新保存后再运行）";
                return false;
            }
            approved = true;
        }
        if (!registry::needs_secret(tool, arguments).isEmpty()) {
            out = "ERROR: 程序里不能输入密码";
            return false;
        }
        out = registry::execute(tool, arguments, approved);
        return !failed_text(out);
    }

    cJSON* load(const String& name) override {
        if (name == kTriggerProgram) return cJSON_Parse(trigger_json.c_str());
        return load_program(name);
    }

    String now() override { return clock_text(time(nullptr), "%H:%M"); }
    bool out_of_time() override { return slice_ && millis() - started_ > kSliceMs; }

private:
    bool is_approved(const String& program, uint32_t print) {
        if (program == kTriggerProgram) {
            cJSON* list = cJSON_Parse(trigger_approved.c_str());
            const bool ok = approved_in(list, print);
            cJSON_Delete(list);
            return ok;
        }
        cJSON* root = read_json(kApproved);
        const bool ok = approved_in(cJSON_GetObjectItemCaseSensitive(root, program.c_str()), print);
        cJSON_Delete(root);
        return ok;
    }

    bool slice_;
    uint32_t started_;
};

String trigger_program(const String& step_json) {
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", kTriggerProgram);
    cJSON* steps = cJSON_AddArrayToObject(p, "steps");
    cJSON* step = cJSON_Parse(step_json.c_str());
    if (step) cJSON_AddItemToArray(steps, step);
    const String out = json_print(p);
    cJSON_Delete(p);
    return out;
}

// Whether `name` or anything it calls waits or asks (decides inline vs queued).
void deep_flags(const String& name, bool& asks, bool& waits, int depth = 0) {
    if (depth > prog::kMaxDepth) return;
    cJSON* p = load_program(name);
    if (!p) return;
    prog::Info info;
    prog::validate(p, info);
    cJSON_Delete(p);
    asks = asks || info.uses_ask;
    waits = waits || info.uses_wait;
    for (const auto& callee : info.callees) deep_flags(callee, asks, waits, depth + 1);
}

void enqueue(const Item& item, bool front = false) {
    std::lock_guard<std::mutex> guard(lock);
    if (queue.size() >= kMaxQueue) {
        runtime_event("prog_queue_full", item.kind + " " + item.a);
        return;
    }
    if (front) queue.insert(queue.begin(), item);
    else queue.push_back(item);
}

String item_json(const Item& item) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", item.kind.c_str());
    cJSON_AddStringToObject(o, "a", item.a.c_str());
    cJSON_AddStringToObject(o, "b", item.b.c_str());
    cJSON_AddStringToObject(o, "c", item.c.c_str());
    cJSON_AddStringToObject(o, "data", item.data.c_str());
    const String out = json_print(o);
    cJSON_Delete(o);
    return out;
}

// One queued run on the worker task.
void job(uint32_t job_id, const String& text) {
    cJSON* o = cJSON_Parse(text.c_str());
    const Item item{json_str(o, "kind"), json_str(o, "a"), json_str(o, "b"), json_str(o, "c"), json_str(o, "data")};
    cJSON_Delete(o);

    DeviceHost host(true);
    auto run = std::make_unique<prog::Run>();
    String label, error;
    int run_id = 0;
    if (item.kind == "trigger") {
        label = item.a;
        host.trigger_json = trigger_program(item.b);
        host.trigger_approved = item.c;
        cJSON* data = cJSON_Parse(item.data.c_str());
        error = run->start(host, kTriggerProgram, "", data);
        cJSON_Delete(data);
    } else if (item.kind == "run") {
        label = item.a;
        error = run->start(host, item.a, item.b, nullptr);
    } else {  // resume
        run_id = item.a.toInt();
        cJSON* saved = read_json(run_path(run_id).c_str(), 16384);
        label = json_str(saved, "label");
        host.trigger_json = json_str(saved, "trigger_json");
        host.trigger_approved = json_str(saved, "trigger_approved");
        error = saved ? run->restore(host, json_str(saved, "state")) : String("运行记录不见了");
        cJSON_Delete(saved);
    }

    ui::set_busy("RUN 程序");
    ui::entry_begin("[程序] " + label);
    prog::Result result;
    if (error.isEmpty()) {
        result = run->resume(host);
    } else {
        result.status = prog::Status::Failed;
        result.error = error;
    }
    String detail;
    for (const auto& line : run->log()) detail += line + "\n";

    const bool keeps_waiting = result.status == prog::Status::Waiting || result.status == prog::Status::Yielded;
    if (keeps_waiting) {
        if (!run_id) run_id = next_run++;
        const time_t resume_at = time(nullptr) + (result.status == prog::Status::Waiting ? result.wait_s : 0);
        cJSON* saved = cJSON_CreateObject();
        cJSON_AddStringToObject(saved, "label", label.c_str());
        cJSON_AddStringToObject(saved, "state", run->save().c_str());
        cJSON_AddStringToObject(saved, "trigger_json", host.trigger_json.c_str());
        cJSON_AddStringToObject(saved, "trigger_approved", host.trigger_approved.c_str());
        cJSON_AddStringToObject(saved, "resume", resume_target(resume_at, run_id).c_str());
        write_json(run_path(run_id), saved);
        cJSON_Delete(saved);
        if (result.status == prog::Status::Yielded) {
            enqueue({"resume", String(run_id), "", "", ""}, true);
            ui::entry_finish(ui::Status::Done, "执行较久，分段继续中…", detail, "[]");
        } else {
            String normalized;
            const String err = registry::watch(resume_target(resume_at, run_id), normalized);
            const String when = clock_text(resume_at, "%H:%M:%S");
            ui::entry_finish(ui::Status::Done, "等待中，" + when + " 继续" + (err.isEmpty() ? String() : "（" + err + "）"),
                             detail, "[]");
        }
        worker::set_result(job_id, "waiting");
    } else {
        if (run_id) storage::remove(run_path(run_id));
        const bool ok = result.status == prog::Status::Done;
        const String summary = ok ? (result.value.isEmpty() ? String("完成") : result.value) : result.error;
        ui::entry_finish(ok ? ui::Status::Done : ui::Status::Failed, summary, detail, "[]");
        if (item.kind != "trigger" && !label.startsWith("触发 #")) record_stat(label, false, ok, summary);
        note_recent(label + (ok ? " 完成：" : " 失败：") + summary);
        worker::set_result(job_id, ok ? result.value : String(), ok ? String() : result.error);
    }
    ui::set_idle();
    runtime_event("prog_run", label + " " + (keeps_waiting ? "waiting" : result.status == prog::Status::Done ? "done" : "failed") +
                                  " steps=" + String(run->executed()));
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
        load_triggers();
        last_arm = millis() - 20000;  // arm right away
    }
    if (millis() - last_arm > 10000) {  // arm triggers and waiting runs (again, if the clock was not set)
        last_arm = millis();
        arm_triggers();
        static bool runs_armed = false;
        if (!runs_armed && time(nullptr) > 1700000000) {
            runs_armed = true;
            arm_runs();
        }
    }

    std::vector<std::pair<String, String>> incoming;
    {
        std::lock_guard<std::mutex> guard(lock);
        incoming.swap(events_in);
    }
    for (const auto& event : incoming) {
        const int tag = event.first.indexOf("#r");
        if (tag > 0) {
            enqueue({"resume", event.first.substring(tag + 2), "", "", ""});
            continue;
        }
        std::vector<Item> due;
        {
            std::lock_guard<std::mutex> guard(lock);
            bool changed = false;
            for (auto it = triggers.begin(); it != triggers.end();) {
                if (it->paused || it->when != event.first) {
                    ++it;
                    continue;
                }
                due.push_back({"trigger", "触发 #" + String(it->id), it->run, it->approved, event.second});
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

    if (worker::busy() || agent::active()) return;  // never while a conversation is open
    Item next;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (queue.empty()) return;
        next = queue.front();
    }
    worker::Job job_spec;
    job_spec.kind = worker::Kind::Callback;
    job_spec.callback = job;
    job_spec.text = item_json(next);
    if (worker::submit(job_spec)) {
        std::lock_guard<std::mutex> guard(lock);
        if (!queue.empty()) queue.erase(queue.begin());
    }
}

// ---------------------------------------------------------------- prog://

String list_text() {
    auto programs = all_programs();
    if (programs.empty()) return "还没有程序。写程序：write prog://名字（格式见 read prog://help）";
    cJSON* stats = stats_root();
    String out = "共 " + String(programs.size()) + " 个程序：\n";
    int n = 0;
    for (cJSON* p : programs) {
        prog::Info info;
        prog::validate(p, info);
        out += String(++n) + ". " + signature(info);
        if (!info.about.isEmpty()) out += " — " + info.about;
        out += " — " + String(info.statements) + " 步";
        if (!info.callees.empty()) {
            out += " — 调用：";
            for (std::size_t i = 0; i < info.callees.size(); ++i) out += (i ? "、" : "") + info.callees[i];
        }
        String when;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (const auto& t : triggers) {
                cJSON* step = cJSON_Parse(t.run.c_str());
                String name, query;
                if (prog::program_target(json_str(step, "exec"), name, query) && name == info.name) {
                    when += (when.isEmpty() ? "" : "、") + describe_when(t.when);
                }
                cJSON_Delete(step);
            }
        }
        out += " — " + (when.isEmpty() ? String("手动运行") : "触发：" + when);
        const cJSON* stat = json_obj(stats, info.name.c_str());
        const double last = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(stat, "last"));
        if (last > 0) {
            out += " — 上次 " + clock_text(static_cast<time_t>(last), "%m-%d %H:%M") +
                   (json_bool(stat, "ok", false) ? " 完成" : " 失败");
        }
        out += "\n";
    }
    cJSON_Delete(stats);
    free_all(programs);
    out.trim();
    return out;
}

String show(const String& name) {
    cJSON* p = load_program(name);
    if (!p) return "ERROR: 没有程序「" + name + "」（read prog://list 查看）";
    prog::Info info;
    prog::validate(p, info);
    String out = "「" + signature(info) + "」" + (info.about.isEmpty() ? String() : " " + info.about) + "\n" +
                 outline(cJSON_GetObjectItemCaseSensitive(p, "steps"), "", 0) + "\n原文：" + json_print(p);
    cJSON_Delete(p);
    return out;
}

String save(const String& name, const String& content, bool approved) {
    cJSON* p = cJSON_Parse(content.c_str());
    if (!p) return "ERROR: content 不是合法的 JSON（格式见 read prog://help）";
    prog::Info info;
    String err = prog::validate(p, info);
    if (err.isEmpty() && info.name.startsWith("~")) err = "程序名不能以 ~ 开头";
    if (err.isEmpty() && !name.isEmpty() && info.name != name) err = "target 里的程序名「" + name + "」和 name「" + info.name + "」不一致";
    const bool existed = storage::exists(program_path(info.name));
    if (err.isEmpty() && !existed) {
        auto programs = all_programs();
        if (programs.size() >= kMaxPrograms) err = "程序已达上限 " + String(kMaxPrograms) + " 个，先清理不用的（read prog://unused）";
        free_all(programs);
    }
    if (err.isEmpty()) {
        DeviceHost host(false);
        err = prog::check_calls(info.name, info.callees, host);
    }
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    if (err.isEmpty()) err = sensitive_steps(cJSON_GetObjectItemCaseSensitive(p, "steps"), prints, lines);
    if (err.isEmpty() && !prints.empty() && !approved) err = "含需要确认的步骤，必须由用户确认后保存";
    if (!err.isEmpty()) {
        cJSON_Delete(p);
        return "ERROR: " + err;
    }
    write_json(program_path(info.name), p);
    cJSON_Delete(p);
    set_approvals(info.name, prints);
    if (!existed) record_stat(info.name, true, true, "");
    String out = (existed ? "已修改程序「" : "已保存程序「") + signature(info) + "」：" + String(info.statements) + " 步";
    if (!info.callees.empty()) {
        out += "，调用 ";
        for (std::size_t i = 0; i < info.callees.size(); ++i) out += (i ? "、" : "") + info.callees[i];
    }
    if (!lines.empty()) out += "，其中 " + String(lines.size()) + " 步已确认";
    out += "。运行：exec prog://" + info.name + "；定时或开机运行：write trigger://new";
    return out;
}

// Who still uses `name`: triggers and programs, "" when nobody.
String users_of(const String& name) {
    String users;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& t : triggers) {
            cJSON* step = cJSON_Parse(t.run.c_str());
            String callee, query;
            if (prog::program_target(json_str(step, "exec"), callee, query) && callee == name) {
                users += (users.isEmpty() ? "" : "、") + String("触发 #") + String(t.id);
            }
            cJSON_Delete(step);
        }
    }
    auto programs = all_programs();
    for (cJSON* p : programs) {
        prog::Info info;
        prog::validate(p, info);
        for (const auto& c : info.callees) {
            if (c == name && info.name != name) users += (users.isEmpty() ? "" : "、") + String("程序「") + info.name + "」";
        }
    }
    free_all(programs);
    return users;
}

String remove(const String& name) {
    if (!storage::exists(program_path(name))) return "ERROR: 没有程序「" + name + "」";
    const String users = users_of(name);
    if (!users.isEmpty()) return "ERROR: 「" + name + "」还被 " + users + " 使用，先删除或修改它们";
    storage::remove(program_path(name));
    set_approvals(name, {});
    forget_stat(name);
    return "已删除程序「" + name + "」";
}

std::vector<String> unused_names() {
    std::vector<String> names;
    auto programs = all_programs();
    cJSON* stats = stats_root();
    const time_t now = time(nullptr);
    for (cJSON* p : programs) {
        const String name = json_str(p, "name");
        const cJSON* stat = json_obj(stats, name.c_str());
        double last = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(stat, "last"));
        if (!(last > 0)) last = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(stat, "created"));
        const bool old = last > 0 && now > 1700000000 && now - static_cast<time_t>(last) > kUnusedSeconds;
        if (old && users_of(name).isEmpty()) names.push_back(name);
    }
    cJSON_Delete(stats);
    free_all(programs);
    return names;
}

String cleanup_list() {
    String out;
    for (const auto& n : unused_names()) out += (out.isEmpty() ? "" : "、") + n;
    return out;
}

String unused_text() {
    const String list = cleanup_list();
    return list.isEmpty() ? String("没有可清理的程序（都还在用，或 30 天内运行过）")
                          : "没被引用且超过 30 天没运行的程序：" + list + "。删除：write prog://cleanup";
}

String cleanup() {
    const auto names = unused_names();
    if (names.empty()) return "没有可清理的程序";
    String out = "已删除 " + String(names.size()) + " 个程序：";
    for (std::size_t i = 0; i < names.size(); ++i) {
        remove(names[i]);
        out += (i ? "、" : "") + names[i];
    }
    return out;
}

String runs_text() {
    String out;
    cJSON* list = cJSON_Parse(storage::list_dir(kRunDir).c_str());
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        const int id = json_str(item, "name").toInt();
        if (id <= 0) continue;
        cJSON* run = read_json(run_path(id).c_str(), 16384);
        const String resume = json_str(run, "resume");
        out += "#" + String(id) + " " + json_str(run, "label") + "：" +
               resume.substring(resume.indexOf(' ') + 1, resume.indexOf('#')) + " 继续\n";
        cJSON_Delete(run);
    }
    cJSON_Delete(list);
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!queue.empty()) out += "排队中：" + String(queue.size()) + " 个\n";
        if (!recent.empty()) {
            out += "最近：\n";
            for (const auto& r : recent) out += "  " + r + "\n";
        }
    }
    out.trim();
    return out.isEmpty() ? String("没有等待中或最近的运行") : out;
}

String run(const String& name, const String& query) {
    cJSON* p = load_program(name);
    if (!p) return "ERROR: 没有程序「" + name + "」（read prog://list 查看）";
    cJSON_Delete(p);
    bool asks = false, waits = false;
    deep_flags(name, asks, waits);
    if (waits || (asks && agent::active())) {
        enqueue({"run", name, query, "", ""});
        return "已排队运行「" + name + "」：它含 " + String(waits ? "wait" : "ask://") +
               "，会在这次对话结束后执行，结果显示在时间线上（read prog://runs 查看）";
    }
    DeviceHost host(false);
    prog::Run r;
    String err = r.start(host, name, query, nullptr);
    prog::Result result;
    if (err.isEmpty()) {
        result = r.resume(host);
    } else {
        result.status = prog::Status::Failed;
        result.error = err;
    }
    const bool ok = result.status == prog::Status::Done;
    record_stat(name, false, ok, ok ? result.value : result.error);
    note_recent(name + (ok ? " 完成" : " 失败：" + result.error));
    String steps;
    for (const auto& line : r.log()) steps += "  " + line + "\n";
    return ok ? "程序「" + name + "」完成，结果：" + (result.value.isEmpty() ? String("（无）") : result.value) +
                    "\n步骤：\n" + steps
              : "ERROR: 程序「" + name + "」失败：" + result.error + "\n步骤：\n" + steps;
}

// ---------------------------------------------------------------- trigger://

String trigger_line(int id) {
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& t : triggers) {
        if (t.id != id) continue;
        cJSON* step = cJSON_Parse(t.run.c_str());
        const String line = "#" + String(t.id) + " " + describe_when(t.when) + " → " + describe_step(step) +
                            (t.paused ? "（已停用）" : t.armed ? "" : "（等待对时）");
        cJSON_Delete(step);
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
    if (ids.empty()) return "还没有触发。新建：write trigger://new，content 如 {\"when\":\"time://every/07:00\",\"run\":{\"exec\":\"prog://早安\"}}";
    String out = "共 " + String(ids.size()) + " 个触发：\n";
    for (const int id : ids) out += trigger_line(id) + "\n";
    out.trim();
    return out;
}

// Validates a trigger's {when, run}; `step` gets the run step as JSON.
String check_trigger(const String& content, String& when, String& step, std::vector<uint32_t>& prints,
                     std::vector<String>& lines) {
    cJSON* t = cJSON_Parse(content.c_str());
    if (!cJSON_IsObject(t)) {
        cJSON_Delete(t);
        return "content 要写成 {\"when\": \"time://every/07:00\", \"run\": {一个步骤}}";
    }
    when = json_str(t, "when");
    const cJSON* run = cJSON_GetObjectItemCaseSensitive(t, "run");
    String err;
    if (when.isEmpty()) err = "缺少 when（如 time://every/07:00、time://in/10m、device://boot）";
    else if (!cJSON_IsObject(run)) err = "缺少 run（一个步骤，如 {\"exec\": \"prog://早安\"}）";
    if (err.isEmpty()) {
        step = json_print(run);
        cJSON* program = cJSON_Parse(trigger_program(step).c_str());
        prog::Info info;
        err = prog::validate(program, info);
        if (err.isEmpty() && (info.uses_wait || info.statements != 1)) err = "run 只能是一个动作；多步请先写成程序，再用 exec prog://名字";
        if (err.isEmpty()) {
            DeviceHost host(false);
            err = prog::check_calls(kTriggerProgram, info.callees, host);
        }
        if (err.isEmpty()) err = sensitive_steps(cJSON_GetObjectItemCaseSensitive(program, "steps"), prints, lines);
        cJSON_Delete(program);
    }
    cJSON_Delete(t);
    return err;
}

String add_trigger(const String& content, bool approved) {
    String when, step;
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    String err = check_trigger(content, when, step, prints, lines);
    if (err.isEmpty() && !prints.empty() && !approved) err = "含需要确认的步骤，必须由用户确认后建立";
    {
        std::lock_guard<std::mutex> guard(lock);
        if (err.isEmpty() && triggers.size() >= kMaxTriggers) err = "触发已达上限 " + String(kMaxTriggers) + " 个";
    }
    String normalized;
    if (err.isEmpty()) err = registry::watch(when, normalized);
    if (!err.isEmpty()) return "ERROR: " + err;
    Trigger t;
    t.when = normalized;
    t.run = step;
    cJSON* approvals = cJSON_CreateArray();
    for (const uint32_t p : prints) cJSON_AddItemToArray(approvals, cJSON_CreateNumber(p));
    t.approved = json_print(approvals);
    cJSON_Delete(approvals);
    t.armed = true;
    {
        std::lock_guard<std::mutex> guard(lock);
        int id = 1;
        for (const auto& x : triggers) id = std::max(id, x.id + 1);
        t.id = id;
        triggers.push_back(t);
        save_triggers();
    }
    return "已新建触发 " + trigger_line(t.id);
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
    if (action != "resume" && !shared) registry::unwatch(when);
    if (action == "resume") arm_triggers();
    if (action == "delete") return "已删除触发 #" + String(id);
    return trigger_line(id);
}

// ---------------------------------------------------------------- Policy and confirmation helpers

bool needs_confirm(const String& program_json) {
    cJSON* p = cJSON_Parse(program_json.c_str());
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    if (p) sensitive_steps(cJSON_GetObjectItemCaseSensitive(p, "steps"), prints, lines);
    cJSON_Delete(p);
    return !prints.empty();
}

bool trigger_needs_confirm(const String& trigger_json) {
    String when, step;
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    check_trigger(trigger_json, when, step, prints, lines);
    return !prints.empty();
}

String save_summary(const String& name, const String& program_json) {
    cJSON* p = cJSON_Parse(program_json.c_str());
    prog::Info info;
    if (p) prog::validate(p, info);
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    if (p) sensitive_steps(cJSON_GetObjectItemCaseSensitive(p, "steps"), prints, lines);
    cJSON_Delete(p);
    String out = "保存程序「" + (info.name.isEmpty() ? name : info.name) + "」（" + String(info.statements) + " 步），需确认：";
    for (std::size_t i = 0; i < lines.size(); ++i) out += (i ? "；" : "") + lines[i];
    return out;
}

String trigger_summary(const String& trigger_json) {
    String when, step;
    std::vector<uint32_t> prints;
    std::vector<String> lines;
    check_trigger(trigger_json, when, step, prints, lines);
    cJSON* s = cJSON_Parse(step.c_str());
    const String out = "新建触发：" + describe_when(when) + " → " + (s ? describe_step(s) : String("?"));
    cJSON_Delete(s);
    return out;
}

}  // namespace vat::programs
