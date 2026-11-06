#pragma once

#include <functional>
#include <vector>

#include "common.h"

// Programs (prog://): step lists the agent writes in JSON and the device runs.
// Validation and a resumable interpreter, with no device calls, so they also
// build on the host (tests/host).  The program package supplies a Host: one
// tool call through Policy and the registry, loading programs, the clock.
// Format and rules: docs/PROGRAMS.zh-CN.md.
namespace vat::prog {

constexpr int kMaxStatements = 40;  // per program, nested ones included
constexpr int kMaxNesting = 4;
constexpr int kMaxExecuted = 200;   // per run, called programs and loops included
constexpr int kMaxRepeat = 10;
constexpr int kMaxDepth = 3;        // program calls
constexpr long kMaxWaitSeconds = 24L * 3600;
constexpr std::size_t kMaxValueBytes = 600;  // a variable's text

class Host {
public:
    virtual ~Host() = default;
    // One action of `program`: `tool` with `arguments` (variables filled in);
    // `step` is the step as written (its approval fingerprint).  false means
    // the step failed and `out` says why.
    virtual bool call(const String& program, const String& tool, const cJSON* arguments, const cJSON* step,
                      String& out) = 0;
    // A program by name, parsed (the caller deletes it), or nullptr.
    virtual cJSON* load(const String& name) = 0;
    virtual String now() = 0;                     // "HH:MM" for {now}
    virtual bool out_of_time() { return false; }  // the time slice of this run is used up
};

struct Input {
    String name;
    String fallback;
    bool required = true;
};

// What validation learned about a program.
struct Info {
    String name;
    String about;
    std::vector<Input> inputs;    // declared, or found as {input.x}
    std::vector<String> callees;  // programs it runs (exec prog://…)
    bool uses_ask = false;
    bool uses_wait = false;
    int statements = 0;
};

// "" when `program` is valid, else one reason the agent can act on.
String validate(const cJSON* program, Info& info);
// "" when `name` calling `callees` closes no cycle and stays within kMaxDepth.
String check_calls(const String& name, const std::vector<String>& callees, Host& host);
// Every action step under `steps`, nested ones included.
void for_each_action(const cJSON* steps, const std::function<void(const String& tool, const cJSON* step)>& visit);
// The arguments an action step passes to its tool, variables left as written
// (what Policy is asked about when the program is saved).  Caller deletes.
cJSON* action_arguments(const cJSON* step, String& tool);
// Identity of a step as written: what an approval is bound to.
uint32_t fingerprint(const cJSON* step);
long parse_wait(const String& text);  // "25m" "90s" "1h30m" -> seconds, -1 when invalid
// "prog://走去?to=x" -> name "走去", query "to=x"; false when not a program target.
bool program_target(const String& target, String& name, String& query);

enum class Status { Done, Waiting, Yielded, Failed };

struct Result {
    Status status = Status::Done;
    String value;     // Done: the last step's result
    String error;     // Failed
    long wait_s = 0;  // Waiting: seconds until it continues
};

// One run of a program: start(), then resume() until it is not Waiting or
// Yielded; save() / restore() carry a waiting run across jobs and reboots.
class Run {
public:
    Run() = default;
    ~Run();
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    // "" when started; `query` is "a=1&b=2"; `event` the trigger's data (may be null).
    String start(Host& host, const String& name, const String& query, const cJSON* event);
    Result resume(Host& host);
    String save() const;
    String restore(Host& host, const String& state);  // "" when restored
    const std::vector<String>& log() const { return log_; }
    int executed() const { return executed_; }

private:
    struct Block {
        String path;  // where the step list is inside the program: "steps", "steps.3.then"
        int index = 0;
        int left = 1;  // repeat passes left, this one included
    };
    struct Frame {
        String name;
        cJSON* program = nullptr;
        cJSON* vars = nullptr;  // as-results, plus "input"
        std::vector<Block> blocks;
        String last;
        String pending_as;            // where a called program's result goes
        bool pending_continue = false;  // that call had on_error: continue
    };

    String enter(Host& host, const String& name, const String& query);
    void drop_frame();
    bool unwind(const String& reason);  // true: a caller continues after a failed call
    String lookup(const Frame& frame, const String& name, Host& host) const;
    String fill(const Frame& frame, const String& text, Host& host, bool query_safe) const;
    cJSON* fill_value(const Frame& frame, const cJSON* value, Host& host) const;
    bool test(const Frame& frame, const cJSON* step, Host& host) const;
    void note(const String& line);

    std::vector<Frame> frames_;
    cJSON* event_ = nullptr;
    String error_;
    int executed_ = 0;
    std::vector<String> log_;
};

}  // namespace vat::prog
