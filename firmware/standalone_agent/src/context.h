#pragma once

#include <Arduino.h>

#include <vector>

// Long context on the TF card (docs/EXTENSIONS.zh-CN.md "长上下文").
//
//   /vat/context/window.jsonl   recent turns as ready-to-send message fragments
//   /vat/context/summary.json   summary of older turns, one message fragment
//   /vat/context/state.json     next output id, current case
//   /vat/out/<id/1000>/<id>.txt full tool outputs (+ .meta.json)
//   /vat/archive/<YYYYMMDD>.jsonl every step of every task
//   /vat/cases/<name>/          notes.md + findings.jsonl
//
// The window and the summary are streamed into the model request straight
// from storage, so their size is bounded by the budget, not by the heap.
// Without a card the window still works with the old 8 KB budget; outputs,
// archive search and cases need the card.
namespace vat::context {

constexpr char kWindow[] = "/vat/context/window.jsonl";
constexpr char kSummary[] = "/vat/context/summary.json";
// The task in progress: its assistant tool_calls and tool results, appended
// as they happen and streamed into each request like the window.  Keeping
// them on storage instead of a growing String is what keeps the heap whole:
// 4 KB results concatenated in RAM fragmented it within a few dozen tasks.
constexpr char kTask[] = "/vat/context/task.jsonl";

void begin();          // after storage::begin()
// memory://out, memory://history and memory://cases belong to the memory
// package; every other memory:// target stays with the local package.
bool owns_target(const String& target);
bool on_card();        // outputs, archive search and cases are available
std::size_t budget();  // bytes of window the request may carry

// ---- window --------------------------------------------------------------
// `fragment` is ",{message},{message}..." for one turn (user first).
bool append_turn(const String& fragment);
std::size_t turns();
std::size_t window_bytes();
// Offset of the first line such that the rest is at most `max_bytes`.
std::size_t tail_offset(std::size_t max_bytes);
bool drop_before(std::size_t offset);      // keep the window from `offset` on
bool write_summary(const String& text);    // replaces the summary fragment
void task_begin();                         // empty the task file
bool task_append(const String& fragment);  // ",{message}"
std::size_t task_bytes();
// Window line = "," user + task file + "," answer.  False when storage failed.
bool commit_task(const String& user_json, const String& answer_json);
void clear();                              // window + summary

// ---- outputs -------------------------------------------------------------
// What a tool result looks like in the conversation: short results as they
// are; long ones are saved whole and replaced by their head plus "out#N".
// `label` names the call (shown in listings).
String compact_result(const String& label, const String& result);
// ssh hands over a complete output it fetched into a cache file; the next
// compact_result() files it instead of the (truncated) text it was given.
void offer_full_output(const String& cache_path, std::size_t bytes);
String read_output(long id, long page, const String& grep);
String list_outputs();

// ---- archive -------------------------------------------------------------
void archive(const String& record_json);
String recent_operations(std::size_t max_bytes);
String search_history(const String& query);

// ---- cases ---------------------------------------------------------------
String current_case();
String attach_case(const String& name);
String detach_case(const String& name);
String list_cases();
String read_case(const String& name);
String write_case(const String& name, const String& content, bool append);
// ",{system message with the current case notes}" or "" when none.
String case_fragment();

}  // namespace vat::context
