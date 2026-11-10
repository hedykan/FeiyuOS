#include "worker.h"
#include "registry.h"
#include "policy.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>

#include "agent.h"
#include "common.h"
#include "config.h"
#include "ops_ssh.h"
#include "ops_tui.h"
#include "storage.h"
#include "stt.h"
#include "ui_state.h"

namespace vat::worker {
namespace {

constexpr uint32_t kStackBytes = 24576;  // peak 15.4-16 KB (libssh + mbedTLS, full E2E); 1.5x headroom (was 32 KB)
constexpr std::size_t kRecordCount = 16;  // text fields live on storage; slots are small

struct Record {
    uint32_t id = 0;
    Kind kind = Kind::AgentText;
    String status = "empty";  // queued | running | done | error | awaiting_approval | cancelled
    String transcript;
    String reply;
    String error;
    String calls = "[]";
    uint32_t stt_ms = 0;
    uint32_t elapsed_ms = 0;
    bool persisted = false;
};

QueueHandle_t queue = nullptr;
TaskHandle_t worker_task = nullptr;
SemaphoreHandle_t records_mutex = nullptr;
Record records[kRecordCount];
std::atomic<uint32_t> next_id{1};
std::atomic<bool> running{false};
std::atomic<uint32_t> job_started{0};
std::atomic<int> queued{0};
uint8_t detail_level = 0;

struct Envelope {
    uint32_t id;
    Job* job;
};

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::AgentPcm: return "agent_pcm";
        case Kind::AgentText: return "agent_text";
        case Kind::TranscribePcm: return "transcribe_pcm";
        case Kind::DraftPcm: return "draft_pcm";
        case Kind::TerminalInput: return "terminal_input";
        case Kind::TerminalObserve: return "terminal_observe";
        case Kind::Approve: return "approve";
        case Kind::Reject: return "reject";
        case Kind::ApproveAll: return "approve_all";
        case Kind::Secret: return "secret";
        case Kind::Abort: return "abort";
        case Kind::Discover: return "discover";
        case Kind::Call: return "call";
        case Kind::Callback: return "callback";
    }
    return "unknown";
}

String record_path(uint32_t id) { return "/vat/cache/jobs/" + String(id % kRecordCount) + ".json"; }

// Agent turns: the text-heavy fields go to storage (a disposable cache read
// back only by GET /api/job, which checks the id) so they do not stay
// resident and fragment the heap.  Other jobs (terminal refresh every 5 s,
// input, drafts, discover) are not written at all: only the newest one keeps
// its text in RAM, enough for a client polling the job it just submitted.
bool agent_kind(Kind kind) {
    return kind == Kind::AgentPcm || kind == Kind::AgentText || kind == Kind::Approve || kind == Kind::Reject ||
           kind == Kind::ApproveAll || kind == Kind::Secret;
}

// release() (common.h), not = String(): that kept each slot's largest text
// ever (13 KB after one E2E round, heap trace 2026-11-08).
void clear_text(Record& record) {
    release(record.transcript);
    release(record.reply);
    release(record.error);
    release(record.calls);
}

void persist_record(Record& record) {
    if (!agent_kind(record.kind)) {
        for (auto& other : records) {
            if (&other != &record && !agent_kind(other.kind) && !other.persisted && other.status != "queued" &&
                other.status != "running") {
                clear_text(other);
            }
        }
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", record.id);
    cJSON_AddStringToObject(root, "transcript", record.transcript.c_str());
    cJSON_AddStringToObject(root, "reply", record.reply.c_str());
    cJSON_AddStringToObject(root, "error", record.error.c_str());
    cJSON* calls = cJSON_Parse(record.calls.c_str());
    cJSON_AddItemToObject(root, "calls", calls ? calls : cJSON_CreateArray());
    storage::overwrite(record_path(record.id), json_print(root));
    cJSON_Delete(root);
    clear_text(record);
    record.persisted = true;
}

void update(uint32_t id, const std::function<void(Record&)>& mutate) {
    xSemaphoreTake(records_mutex, portMAX_DELAY);
    Record& record = records[id % kRecordCount];
    if (record.id != id) {
        clear_text(record);  // record = Record{} would keep the old buffers (see release)
        record = Record{};
        record.id = id;
    }
    mutate(record);
    xSemaphoreGive(records_mutex);
}

String calls_json(const std::vector<agent::Call>& calls) {
    cJSON* list = cJSON_CreateArray();
    for (const auto& call : calls) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", call.name.c_str());
        cJSON* args = cJSON_Parse(call.arguments.c_str());
        cJSON_AddItemToObject(item, "arguments", args ? args : cJSON_CreateObject());
        cJSON_AddStringToObject(item, "decision", call.decision.c_str());
        cJSON_AddStringToObject(item, "result", utf8_head(call.result, 1200).c_str());
        cJSON_AddItemToArray(list, item);
    }
    const String json = json_print(list);
    cJSON_Delete(list);
    return json;
}

// A remote program waiting for a password: last visible line ends with a
// password prompt.  The SSH page then switches its input to masked mode.
bool password_prompt(const String& screen) {
    String last = screen;
    while (last.endsWith("\n") || last.endsWith(" ")) last.remove(last.length() - 1);
    const int newline = last.lastIndexOf('\n');
    if (newline >= 0) last = last.substring(newline + 1);
    String lowered = last;
    lowered.toLowerCase();
    const bool ends_like_prompt = lowered.endsWith(":") || last.endsWith("：");
    return ends_like_prompt && (lowered.indexOf("password") >= 0 || lowered.indexOf("passphrase") >= 0 ||
                                last.indexOf("密码") >= 0);
}

// The SSH page shows the session the agent connected.  Opening terminals is
// the agent's job (session connect / create / start_agent); without one the
// page only explains how to get one.
// The last session ended under us (closed, killed, host rebooted): keep
// saying so until another one is opened, rather than a bare "no terminal".
bool session_ended = false;

void refresh_terminal() {
    const String hint = "\n回到 Agent 页说“连接" + config::get().default_profile + "”";
    if (!tui::has_current()) {
        ui::set_terminal("未连接", String(session_ended ? "终端会话已结束" : "没有活动终端") + hint);
        return;
    }
    const String raw = tui::observe("tui://current");
    if (raw == tui::kSessionGone) {
        session_ended = true;
        ui::set_terminal("未连接", "终端会话已结束" + hint);
        return;
    }
    session_ended = false;
    const auto session = tui::current();
    ui::set_terminal(session.profile + " · " + session.name,
                     detail_level == 0 ? tui::compact_screen(raw) : utf8_tail(raw, 1400), password_prompt(raw));
}

// After an agent turn that touched the terminal, refresh the SSH page.
void mirror_to_terminal(const std::vector<agent::Call>& calls) {
    for (const auto& call : calls) {
        const bool executed = call.decision != "deny" && call.decision != "discover" && call.decision != "rejected";
        if (executed && (call.name == "observe" || call.name == "input" || call.name == "session")) {
            delay(600);
            refresh_terminal();
            return;
        }
    }
}

// Step lines for the detail view: "exec 开发机 $ df -h" + first result line.
String steps_json(const std::vector<agent::Call>& calls) {
    cJSON* list = cJSON_CreateArray();
    for (const auto& call : calls) {
        if (call.decision == "discover") continue;
        cJSON* args = cJSON_Parse(call.arguments.c_str());
        String what = call.name;
        const String target = json_str(args, "target");
        if (!target.isEmpty()) what += " " + target;
        if (call.name == "exec") {
            what += " $ " + json_str(args, "program");
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(args, "args")) {
                if (cJSON_IsString(item)) what += String(" ") + item->valuestring;
            }
        } else if (call.name == "session") {
            what += " " + json_str(args, "action");
        }
        cJSON_Delete(args);
        cJSON* step = cJSON_CreateObject();
        cJSON_AddStringToObject(step, "what", utf8_head(what, 160).c_str());
        cJSON_AddStringToObject(step, "decision", call.decision.c_str());
        cJSON_AddStringToObject(step, "result", utf8_head(call.result, 600).c_str());
        cJSON_AddItemToArray(list, step);
    }
    const String json = json_print(list);
    cJSON_Delete(list);
    return json;
}

void finish_agent(uint32_t id, const agent::Outcome& outcome) {
    if (outcome.ok && outcome.paused) {
        ui::set_pending(outcome.pending);
        const auto& pending = outcome.pending.front();
        ui::entry_update(pending.secret_prompt.isEmpty() ? ui::Status::Confirm : ui::Status::Secret,
                         pending.secret_prompt.isEmpty() ? pending.summary : pending.secret_prompt);
    } else if (outcome.ok) {
        ui::entry_finish(ui::Status::Done, outcome.reply, outcome.detail, steps_json(outcome.calls));
    } else {
        ui::entry_finish(outcome.error == "已取消" ? ui::Status::Cancelled : ui::Status::Failed, outcome.error,
                         outcome.detail, steps_json(outcome.calls));
    }
    if (outcome.ok) mirror_to_terminal(outcome.calls);
    const String calls = calls_json(outcome.calls);
    update(id, [&](Record& record) {
        record.reply = outcome.reply;
        record.error = outcome.error;
        record.calls = calls;
        record.status = !outcome.ok ? (outcome.error == "已取消" ? "cancelled" : "error")
                        : !outcome.paused ? "done"
                        : outcome.pending.front().secret_prompt.isEmpty() ? "awaiting_approval" : "awaiting_secret";
    });
}

String task_transcript;  // utterance of the task in progress (shown while resuming)

void run_agent(uint32_t id, const String& transcript) {
    task_transcript = transcript;
    ui::set_stage("THINK");
    update(id, [&](Record& record) { record.transcript = transcript; });
    const auto outcome = agent::run(transcript, [](const String& stage) { ui::set_stage(stage); });
    finish_agent(id, outcome);
    if (!outcome.paused) agent::maintain();  // the answer is already on screen
}

// The timeline entry is created before transcription so the user sees the
// request immediately; its text is filled in once the transcript is known.
bool transcribe(uint32_t id, const String& path, String& transcript) {
    ui::set_stage("STT");
    const auto result = stt::transcribe_pcm(path);
    update(id, [&](Record& record) {
        record.stt_ms = result.elapsed_ms;
        if (result.ok) record.transcript = result.text;
    });
    if (!result.ok) {
        const bool cancelled = cancel::requested();
        ui::entry_finish(cancelled ? ui::Status::Cancelled : ui::Status::Failed, result.text, result.text, "[]");
        update(id, [&](Record& record) {
            record.status = cancelled ? "cancelled" : "error";
            record.error = result.text;
        });
        return false;
    }
    transcript = result.text;
    return true;
}

void handle(uint32_t id, Job& job) {
    const unsigned long started = millis();
    heap_caps_monitor_local_minimum_free_size_start();  // this job's own low water mark
    update(id, [&](Record& record) {
        record.kind = job.kind;
        record.status = "running";
    });
    switch (job.kind) {
        case Kind::AgentPcm: {
            ui::set_busy("STT");
            ui::entry_begin("…");
            String transcript;
            if (transcribe(id, job.path, transcript)) {
                ui::entry_retext(transcript);
                run_agent(id, transcript);
            }
            break;
        }
        case Kind::AgentText:
            ui::set_busy("THINK");
            ui::entry_begin(job.text);
            run_agent(id, job.text);
            break;
        case Kind::TranscribePcm: {
            ui::set_busy("STT");
            ui::entry_begin("…");
            String transcript;
            if (transcribe(id, job.path, transcript)) {
                ui::entry_retext(transcript);
                ui::entry_finish(ui::Status::Text, "仅转写，未执行", transcript, "[]");
                update(id, [&](Record& record) {
                    record.status = "done";
                    record.reply = transcript;
                });
            }
            break;
        }
        case Kind::DraftPcm: {
            ui::set_quiet_busy(true);
            const auto result = stt::transcribe_pcm(job.path);
            if (result.ok) {
                if (job.text == "agent") ui::append_agent_draft(result.text);
                else ui::set_draft(result.text);
                update(id, [&](Record& record) {
                    record.status = "done";
                    record.reply = result.text;
                });
            } else {
                ui::set_notice(result.text, true);
                update(id, [&](Record& record) {
                    record.status = "error";
                    record.error = result.text;
                });
            }
            ui::set_quiet_busy(false);
            break;
        }
        case Kind::TerminalInput: {
            ui::set_quiet_busy(true);
            const String result = tui::send_input("tui://current", job.text, job.keys, job.submit);
            wipe(job.text);  // may be a password typed into a remote prompt
            delay(result == "已发送到当前 TUI" ? 700 : 0);
            refresh_terminal();
            update(id, [&](Record& record) {
                record.status = result == "已发送到当前 TUI" ? "done" : "error";
                record.reply = result;
            });
            ui::set_quiet_busy(false);
            break;
        }
        case Kind::TerminalObserve: {
            ui::set_quiet_busy(true);
            refresh_terminal();
            const auto model = ui::snapshot();
            update(id, [&](Record& record) {
                record.status = "done";
                record.reply = model.terminal_text;
            });
            ui::set_quiet_busy(false);
            break;
        }
        case Kind::Approve:
        case Kind::Reject:
        case Kind::ApproveAll:
        case Kind::Secret: {
            if (!agent::paused()) {
                update(id, [&](Record& record) {
                    record.status = "error";
                    record.error = "没有等待中的任务";
                });
                break;
            }
            const agent::Answer answer = job.kind == Kind::Approve      ? agent::Answer::Approve
                                         : job.kind == Kind::Reject     ? agent::Answer::Reject
                                         : job.kind == Kind::ApproveAll ? agent::Answer::ApproveAll
                                                                        : agent::Answer::Secret;
            ui::set_pending({});
            ui::set_busy("RUN");
            ui::set_stage("RUN");
            update(id, [&](Record& record) { record.transcript = task_transcript; });
            const auto outcome =
                agent::resume(answer, job.text, [](const String& stage) { ui::set_stage(stage); });
            wipe(job.text);  // the secret is gone once the step has run
            finish_agent(id, outcome);
            if (!outcome.paused) agent::maintain();
            break;
        }
        case Kind::Call: {  // one call, only when Policy allows it without asking
            ui::set_quiet_busy(true);
            cJSON* request = cJSON_Parse(job.text.c_str());
            const String tool = json_str(request, "tool");
            const cJSON* arguments = json_obj(request, "arguments");
            String reply, error;
            if (tool.isEmpty() || !arguments) {
                error = "需要 {tool, arguments}";
            } else {
                const auto verdict = policy::evaluate(tool, arguments);
                if (verdict.decision != policy::Decision::Allow) error = "未执行（" + verdict.reason + "）";
                else if (!registry::needs_secret(tool, arguments).isEmpty()) error = "未执行（需要密码）";
                else reply = registry::execute(tool, arguments, false);
            }
            cJSON_Delete(request);
            set_result(id, reply, error);
            ui::set_quiet_busy(false);
            break;
        }
        case Kind::Callback:
            if (job.callback) job.callback(id, job.text);
            break;
        case Kind::Discover: {
            ui::set_quiet_busy(true);
            cJSON* arguments = cJSON_Parse(job.text.c_str());
            const String result = arguments ? agent::discover_result(arguments) : String("{\"error\":\"bad arguments\"}");
            cJSON_Delete(arguments);
            update(id, [&](Record& record) {
                record.status = "done";
                record.reply = result;
            });
            ui::set_quiet_busy(false);
            break;
        }
        case Kind::Abort: {
            agent::abort_task();
            ui::set_pending({});
            ui::entry_finish(ui::Status::Cancelled, "已取消", "", "[]");
            update(id, [&](Record& record) { record.status = "cancelled"; });
            break;
        }
    }
    ui::set_idle_if_quiet();
    update(id, [&](Record& record) {
        record.elapsed_ms = millis() - started;
        if (record.status == "running") record.status = "done";
        persist_record(record);
    });
    const std::size_t job_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    if (job.kind != Kind::TerminalObserve) {
        runtime_event("job_done", String(kind_name(job.kind)) + " id=" + id + " " + (millis() - started) + "ms min=" +
                                      String(static_cast<unsigned>(job_min)));
    }
}

void task_main(void*) {
    Envelope envelope;
    while (true) {
        if (xQueueReceive(queue, &envelope, portMAX_DELAY) != pdTRUE) continue;
        job_started = millis();
        running = true;
        --queued;
        cancel::reset();
        handle(envelope.id, *envelope.job);
        ssh::release();
        cancel::clear_socket();
        delete envelope.job;
        running = false;
    }
}

}  // namespace

void begin() {
    // The task watchdog's idle-task check reset the device while the card was
    // merely slow (the SD library busy-waits on core 0; Launcher for the
    // Cardputer removed its watchdogs for the same reason).  The worker is
    // supervised instead: main loop restarts the device if one job runs past
    // kStuckJobMs (agent tasks time out after 45 s).
    const esp_task_wdt_config_t watchdog = {.timeout_ms = 5000, .idle_core_mask = 0, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&watchdog) != ESP_OK) runtime_event("watchdog", "reconfigure failed");
    queue = xQueueCreate(2, sizeof(Envelope));
    records_mutex = xSemaphoreCreateMutex();
    // Core 0 with lwIP: TLS writes are drained as they are made.
    xTaskCreatePinnedToCore(task_main, "vat-worker", kStackBytes, nullptr, 1, &worker_task, 0);
}

bool busy() { return running.load() || queued.load() > 0; }

uint32_t running_ms() { return running.load() ? millis() - job_started.load() : 0; }

void keep_alive() { job_started = millis(); }

uint32_t submit(const Job& job) {
    if (busy()) return 0;
    const uint32_t id = next_id++;
    update(id, [&](Record& record) {
        record.kind = job.kind;
        record.status = "queued";
    });
    Envelope envelope{id, new Job(job)};
    ++queued;
    if (xQueueSend(queue, &envelope, 0) != pdTRUE) {
        --queued;
        delete envelope.job;
        return 0;
    }
    return id;
}

void set_result(uint32_t id, const String& reply, const String& error) {
    update(id, [&](Record& record) {
        record.status = error.isEmpty() ? "done" : "error";
        if (!record.persisted) {
            record.reply = reply;
            record.error = error;
            return;
        }
        // A program run reports after the conversation that started it was
        // saved to the card and its text dropped: update the saved copy.
        // Putting the text back in RAM kept ~1 KB per program run until the
        // slot came round again (13 KB after one E2E round, heap trace 2026-11-08).
        cJSON* stored = cJSON_Parse(storage::read_text(record_path(id), 16384).c_str());
        if (!stored) stored = cJSON_CreateObject();
        cJSON_DeleteItemFromObject(stored, "reply");
        cJSON_DeleteItemFromObject(stored, "error");
        cJSON_AddStringToObject(stored, "reply", reply.c_str());
        cJSON_AddStringToObject(stored, "error", error.c_str());
        storage::overwrite(record_path(id), json_print(stored));
        cJSON_Delete(stored);
    });
}

String record_json(uint32_t id) {
    xSemaphoreTake(records_mutex, portMAX_DELAY);
    const Record record = records[id % kRecordCount];
    xSemaphoreGive(records_mutex);
    if (record.id != id) return "{\"error\":\"unknown job\"}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", record.id);
    cJSON_AddStringToObject(root, "kind", kind_name(record.kind));
    cJSON_AddStringToObject(root, "status", record.status.c_str());
    cJSON* stored = record.persisted ? cJSON_Parse(storage::read_text(record_path(id), 16384).c_str()) : nullptr;
    if (stored && json_int(stored, "id", 0) != static_cast<long>(id)) {
        cJSON_Delete(stored);
        stored = nullptr;
    }
    cJSON_AddStringToObject(root, "transcript", stored ? json_str(stored, "transcript").c_str() : record.transcript.c_str());
    cJSON_AddStringToObject(root, "reply", stored ? json_str(stored, "reply").c_str() : record.reply.c_str());
    cJSON_AddStringToObject(root, "error", stored ? json_str(stored, "error").c_str() : record.error.c_str());
    cJSON* calls = stored ? cJSON_DetachItemFromObject(stored, "calls") : cJSON_Parse(record.calls.c_str());
    cJSON_AddItemToObject(root, "calls", calls ? calls : cJSON_CreateArray());
    cJSON_Delete(stored);
    cJSON_AddNumberToObject(root, "stt_ms", record.stt_ms);
    cJSON_AddNumberToObject(root, "elapsed_ms", record.elapsed_ms);
    const String json = json_print(root);
    cJSON_Delete(root);
    return json;
}

uint8_t& terminal_detail() { return detail_level; }

uint32_t stack_free_min() { return worker_task ? uxTaskGetStackHighWaterMark(worker_task) : 0; }

}  // namespace vat::worker
