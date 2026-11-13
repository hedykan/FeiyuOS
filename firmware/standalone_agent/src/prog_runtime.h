#pragma once

#include <Arduino.h>
#include <time.h>
#include <cJSON.h>

#include <vector>

#include "lua_meta.h"

// Lua programs and packages on the card (/vat/things/lua/名字.lua), their
// index and confirmations, and triggers that run a program on a time or an
// event.  The language is lua_vm; the header is lua_meta.
namespace vat::programs {

// The event bus and the UI loop: triggers queue runs, which start on the
// worker when nothing else is running.
void on_event(const String& target, const String& data);
void loop();
// How many programs have failed three or more background runs in a row (since boot).
int failing();

// prog:// (programs)
String list_text();
String show(const String& name);
String show_part(const String& name, const String& query);  // ?grep=关键词 | ?lines=开始-结束
// write prog://名字/patch: replaces parts of the saved source (see prog_runtime.cpp).
String patch(const String& name, const String& text, bool approved);
bool patch_needs_confirm(const String& name, const String& text);
String patch_summary(const String& name, const String& text);
String save(const String& name, const String& source, bool approved);
String remove(const String& name);
// From a tool call or the program page: a foreground run (the console;
// `stdin_text` lines are what io.read gets, then the end).
String run(const String& name, const String& query, const String& stdin_text = String());
// The program page's Enter on a program: runs it on the worker; false when busy.
bool launch(const String& name);
// Programs the page lists (packages excluded): name, about and when it was
// last used (run, saved or changed on the card; seconds, 0 when unknown).
struct ProgramInfo {
    String name;
    String about;
    time_t used = 0;
};
std::vector<ProgramInfo> runnable();
bool needs_confirm(const String& source);              // declares uses: confirm the save
String save_summary(const String& name, const String& source);
String legacy_text();                                  // old JSON programs, to be translated
String legacy_show(const String& name);

// trigger://
String triggers_text();
String add_trigger(const String& content);
String trigger_action(int id, const String& action, bool approved);  // pause | resume | delete
String trigger_line(int id);
// write trigger://clean: removes triggers whose program is gone and those that
// paused themselves after failing (confirmed).
String trigger_clean(bool approved);
String trigger_clean_summary();

// pkg:// (everything on the card: programs and packages)
String scan();
String pkg_list();
String pkg_action(const String& name, const String& action, bool approved);  // enable | disable | delete
String pkg_summary(const String& name, const String& action);
bool pkg_needs_confirm(const String& name, const String& action);

// Card packages (@package files, enabled): what the card package serves.
struct PackageInfo {
    String name;
    String target;  // "weather://"
    String usage;
    String keywords;
    String about;
};
std::vector<PackageInfo> card_packages();
// Changes whenever the index or a confirmation does (reload card_packages()).
uint32_t generation();
// Installing (pkg_store): the header of the card's `name`, false when none.
bool installed(const String& name, lua_meta::Header& header);
// Puts a downloaded, checked `file` in place as `name`, the current one kept
// as old/名字.lua; confirms `header` when `enable`, else it waits for enable.
// "" or the error.
String adopt(const String& name, const String& file, const lua_meta::Header& header, bool enable);
// Swaps `name` with its kept previous version.
String rollback(const String& name);
bool has_previous(const String& name);

// One tool call to a card package: its file's tool-named function.
String run_package(const String& name, const String& tool, const cJSON* arguments);

}  // namespace vat::programs
