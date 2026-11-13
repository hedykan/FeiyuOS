#pragma once

#include <Arduino.h>

#include <vector>

namespace vat::worker {

enum class Kind : uint8_t {
    AgentPcm,         // voice → STT → agent
    AgentText,        // typed/injected text → agent
    TranscribePcm,    // TEXT mode: voice → transcript only
    DraftPcm,         // SSH page Fn+Enter: voice → terminal draft
    TerminalInput,    // SSH page keyboard → tmux
    TerminalObserve,  // refresh SSH page
    Approve,          // Y: run the paused step, then continue the task
    Reject,           // N: skip it; the model decides what to do next
    ApproveAll,       // A: approve this and every later step of the task
    Secret,           // masked input submitted (text = the secret, never logged)
    Abort,            // Esc while paused: drop the task
    Discover,         // one read-only discover call (prompt lab); text = arguments JSON
    Call,             // one tool call Policy allows outright (E2E, debugging); text = {tool, arguments}
    Callback,         // a package's own work on the worker task; text is passed to `callback`
};

struct Job {
    Kind kind = Kind::AgentText;
    String text;
    String path;
    std::vector<String> keys;
    bool submit = false;
    void (*callback)(uint32_t id, const String& text) = nullptr;  // Kind::Callback
};

void begin();
bool busy();
// How long the current job has been running (0 when idle).
uint32_t running_ms();
// The running job is alive (a program waiting for the user): restarts its stuck clock.
void keep_alive();
constexpr uint32_t kStuckJobMs = 180000;
// Returns the job id, or 0 when another job is running/queued.
uint32_t submit(const Job& job);
// Result of the running job (Callback jobs report through this).
void set_result(uint32_t id, const String& reply, const String& error = String());
// JSON record of a job: status, transcript, reply, calls, timings.
String record_json(uint32_t id);
uint8_t& terminal_detail();
uint32_t stack_free_min();  // bytes never touched on the worker stack
// A finer step inside the running job ("ssh-open", "ssh-fetch" …): job_done's
// steps(KB) list shows the low water mark of each.  Does nothing off the worker task.
void heap_step(const String& name);

// A running job asks the user on the device and waits (worker task only): the
// question shows as the pending step (Agent page Y/N/A, "pending" in the
// status), the job's status is "asking" meanwhile, and Y/N/A or the web
// actions approve, reject and approve_all answer it.  Esc, or no answer
// within kAskMs: No.
enum class Answer : uint8_t { Yes, No, All };
constexpr uint32_t kAskMs = 60000;
Answer ask(const String& summary, const String& reason);
bool asking();
// approve / reject / approve_all for a job that is asking: its id; 0 when none is.
uint32_t answer(const String& action);

}  // namespace vat::worker
