#pragma once

#include <Arduino.h>

// Device side of the program package: programs and triggers on storage,
// approvals, the run queue and jobs on the worker.  The language itself is in
// prog_engine (portable); the package glue is packages/program_package.cpp.
namespace vat::programs {

void on_event(const String& target, const String& data);  // event-bus handler: queues only
void loop();  // UI task: load once, arm triggers, start queued runs when the worker is free

// prog://
String list_text();
String show(const String& name);
String unused_text();
String runs_text();
// Stops a run waiting in a `wait`: drops its timer and its saved state; the
// program stays.  `id` as listed by runs_text.
String cancel_run(int id);
String save(const String& name, const String& content, bool approved);
String remove(const String& name);
String cleanup();
String run(const String& name, const String& query);  // from a tool call: now, or queued

// trigger://
String triggers_text();
String add_trigger(const String& content, bool approved);
String trigger_action(int id, const String& action, bool approved);  // pause | resume | delete

// For Policy and the confirmation screen.
bool needs_confirm(const String& program_json);   // a step Policy would ask about
bool trigger_needs_confirm(const String& trigger_json);
String save_summary(const String& name, const String& program_json);
String trigger_summary(const String& trigger_json);
String trigger_line(int id);  // "#3 每天 07:00 → 运行 早安", "" when unknown
String cleanup_list();        // names cleanup would delete, "、"-joined

}  // namespace vat::programs
