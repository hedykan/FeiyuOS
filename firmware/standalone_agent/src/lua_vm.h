#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <vector>

#include "lua_meta.h"

// Lua programs: one VM per run, on the worker, closed when the script ends.
// The VM lives in a private heap (one block taken at start, more blocks if it
// grows, all given back at the end), so its thousands of small objects never
// mix with the rest of the heap.  The script sees the base, string, table,
// math, utf8 and coroutine libraries and `vat`, whose calls go through Policy
// and the package registry like the model's tool calls.
namespace vat::luavm {

constexpr std::size_t kFirstArena = 24 * 1024;  // a typical script peaks at 11-20 KB
constexpr std::size_t kGrowArena = 12 * 1024;
constexpr std::size_t kMemoryLimit = 48 * 1024;
constexpr uint32_t kMaxInstructions = 20000000;
constexpr uint32_t kMaxMs = 150000;   // a job past 180 s restarts the device
constexpr uint32_t kMaxSleepMs = 60000;
constexpr int kMaxDepth = 3;          // programs calling programs
constexpr std::size_t kMaxOutput = 2048;

struct Options {
    // Confirmed uses: a call Policy would ask about runs when they cover it.
    const std::vector<lua_meta::Use>* uses = nullptr;
    const cJSON* input = nullptr;  // the script's `input` table (strings)
    // A package: after the chunk, call the global function of this name as
    // entry(input.target, input); a table it returns is the result as JSON.
    const char* entry = nullptr;
    // A run the user started (the program page): print also goes to the
    // console, io.read / vat.readline / vat.key read the keyboard, and the
    // limits are 30 minutes and kForegroundInstructions (` interrupts).
    bool foreground = false;
};
constexpr uint32_t kForegroundMs = 30 * 60 * 1000;
constexpr uint64_t kForegroundInstructions = 200000000;

struct Stats {
    std::size_t peak = 0;    // bytes the script held at most
    std::size_t arena = 0;   // bytes taken from the heap for it
    uint32_t instructions = 0;
    uint32_t ms = 0;
};

// Runs `source` (text only).  print() and vat.log() go to `output`; the
// script's return value, as text, to `result`.  false with `error`.
bool run(const String& source, const String& name, const Options& options, String& output, String& result,
         String& error, Stats& stats);
// Compiles without running: "" or the syntax error.
String compile(const String& source, const String& name);

constexpr uint32_t kFrameInstructions = 100000;  // a canvas update, per frame

// A VM kept between calls: a canvas's update, run once per frame on the UI
// task while the canvas is open.  Only the Lua libraries and vat.stream /
// json / tojson / millis / log: no tool calls (they would hold the screen).
class Script {
public:
    ~Script() { close(); }
    // `vars` (a JSON object) become globals; `update` is compiled once.
    // "" or the error.
    String open(const String& vars_json, const String& update);
    String step(long t);  // sets t, runs the update once: "" or the error
    // A global as text: integers plainly, other numbers to 6 decimals without
    // trailing zeros; false when it is nil (not a variable).
    bool text(const String& name, String& out);
    void close();
    bool active() const { return state_ != nullptr; }

private:
    void* state_ = nullptr;    // lua_State*
    void* context_ = nullptr;  // the VM's private heap
    int update_ = -2;          // registry reference of the compiled update (LUA_NOREF)
};

}  // namespace vat::luavm
