#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

struct cJSON;

namespace vat::agent {

struct Call {
    String name;
    String arguments;  // compact JSON
    String result;
    String decision;   // allow | approved | approved_all | rejected | deny | discover | secret_cancelled | ask | secret (waiting)
};

// The step a paused task is waiting on.
struct Pending {
    String tool;
    String arguments;      // compact JSON
    String reason;         // why confirmation is needed (empty for a secret)
    String summary;        // one line: what will run
    String secret_prompt;  // non-empty: waiting for a secret typed on the device
    int step = 0;          // 1-based step number within the task
};

struct Outcome {
    bool ok = false;       // finished with a reply (also true while paused)
    bool paused = false;   // waiting for Y/N/A or a secret
    String reply;          // short text shown on the Agent page
    String detail;         // raw tool results of the task
    String error;
    std::vector<Call> calls;
    std::vector<Pending> pending;  // the waiting step when paused
    int rounds = 0;
    int steps = 0;
    uint32_t elapsed_ms = 0;
};

enum class Answer { Approve, Reject, ApproveAll, Secret };

using Progress = std::function<void(const String& stage)>;

void begin();  // restores bounded conversation history from storage

// Starts a task: model rounds with tool results fed back (up to 6 steps)
// until a final text reply.  Pauses when a step needs confirmation or a
// secret; resume() continues from exactly that step.
// `unattended`: nobody is there to confirm: calls that need confirmation or a
// secret are refused (recorded as such) instead of pausing the task.
Outcome run(const String& transcript, const Progress& progress, bool unattended = false);
Outcome resume(Answer answer, const String& secret, const Progress& progress);
bool paused();
bool active();  // a task is running or paused (a conversation is open)
void abort_task();  // drop a paused task (Esc)

// After a finished task: when the context window is over budget, fold its
// oldest part into the summary (one extra model call).  Runs on the worker
// once the answer is already on screen.
void maintain();

void clear_history();
// What the model receives for one discover call (worker task only: catalog
// packages probe remote programs over SSH).  Also served to the prompt lab.
String discover_result(const cJSON* arguments);
std::size_t history_messages();  // remembered turns

}  // namespace vat::agent
