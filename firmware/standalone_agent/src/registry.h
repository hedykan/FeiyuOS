#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <vector>

// Capability packages.
//
// The model only ever sees the seven tools (discover/read/write/exec/observe/
// input/session).  Everything the device can do is a package registered here:
// a package describes itself for `discover` and claims the tool calls whose
// target scheme / action it owns.  See docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md.
namespace vat::registry {

struct Manifest {
    const char* id;                       // stable, unique, lowercase
    const char* summary;                  // one line for discover(capabilities)
    const char* keywords;                 // Chinese/English search terms
    std::vector<const char*> commands;    // audited command prefixes (catalogs)
    const char* availability_program = nullptr;  // probed on ssh:// targets
};

class Package {
public:
    virtual ~Package() = default;
    virtual const Manifest& manifest() const = 0;

    // True when this package owns the call.  Must match the target scheme or
    // session action exactly; never claim unknown targets.
    virtual bool supports(const String& tool, const cJSON* arguments) const { return false; }

    // Runs an owned call after Policy allowed it (approved=true only after the
    // user confirmed an ASK).  Returns the text result; failures are text too.
    virtual String execute(const String& tool, const cJSON* arguments, bool approved) { return {}; }

    // Secrets (passwords, passphrases, tokens) never come from the model.  A
    // package that needs one returns the prompt shown on the device's masked
    // input ("开发机 · alice 的 sudo 密码"); "" when none is needed.  The
    // typed value then arrives in execute_secret() and must not be logged,
    // returned or stored except where the operation itself needs it.
    virtual String needs_secret(const String& tool, const cJSON* arguments) { return {}; }
    virtual String execute_secret(const String& tool, const cJSON* arguments, bool approved, const String& secret) {
        return execute(tool, arguments, approved);
    }

    // How the model should use this package, appended to the system prompt in
    // registration order.  One or two sentences: the targets and the rules the
    // model cannot learn from discover.  JSON-safe text only (no double quote,
    // backslash or control characters); an unsafe text is dropped at add().
    virtual const char* usage() const { return nullptr; }

    // True when this owned write is local and undoable (screen, sound, LED,
    // notes, timers, TV remote): Policy runs it without asking.  Never for
    // remote hosts, networks, radios, files or anything physical beyond these.
    virtual bool reversible(const String& tool, const cJSON* arguments) const { return false; }

    // Help-page entries: what can be set or done, with an example sentence.
    // `value` is the current setting ("" for capabilities).
    struct Hint {
        String name;
        String value;
        String example;
    };
    virtual void hints(std::vector<Hint>& out) {}

    // Appends safe resource summaries ({target, kind, ...}) for
    // discover(resources); decrement `remaining` per item.
    virtual void resources(cJSON* list, int& remaining) {}

    // Events this package can emit (targets such as "time://every/07:00"), for
    // discover and for whoever builds on the event bus.
    struct TriggerSpec {
        const char* target;
        const char* description;
    };
    virtual void triggers(std::vector<TriggerSpec>& out) const {}

    // Start emitting `target` when it is an event of this package: return true
    // and leave `error` empty when accepted (`normalized` is the form to keep,
    // e.g. time://in/10m becomes an absolute time://at/...), or set `error`.
    // false: not this package's event.  Events emitted anyway just accept.
    virtual bool watch(const String& target, String& normalized, String& error) { return false; }
    virtual void unwatch(const String& target) {}

    // Text shown when an owned call waits for confirmation ("" = the default
    // one-line summary).
    virtual String confirm_summary(const String& tool, const cJSON* arguments) const { return {}; }

    // Periodic work on the UI task (cheap; called every loop).
    virtual void loop() {}

    // Remote availability for catalog packages; nullptr manifest field = n/a.
    virtual bool available(const String& profile) { return false; }

    // Risk of a catalog command: "read_only" or "confirmation_required".
    virtual const char* command_risk(const String& command) const { return "read_only"; }

    // Extra search text for one catalog command.
    virtual const char* search_text(const String& command) const { return ""; }
};

// Appends {target, kind} to a discover(resources) list and decrements
// `remaining`; false (nothing added) once the list is full.
bool add_resource(cJSON* list, int& remaining, const char* target, const char* kind);

// Registers a package; duplicate ids are rejected (returns false).
bool add(Package* package);
const std::vector<Package*>& packages();
// The packages' usage() texts in registration order (system prompt parts).
const std::vector<const char*>& usages();

// Explicit, audited list of first-party packages (src/packages/builtin.cpp).
void register_builtin();

String discover(const cJSON* arguments);
String needs_secret(const String& tool, const cJSON* arguments);
// Whether the package that owns this call declares it local and reversible.
bool reversible(const String& tool, const cJSON* arguments);
String execute(const String& tool, const cJSON* arguments, bool approved, const String& secret = String());
std::vector<Package::Hint> hints();

// Event hooks, dispatched to whichever package owns the target.
String watch(const String& target, String& normalized);  // "" when accepted
void unwatch(const String& target);
String confirm_summary(const String& tool, const cJSON* arguments);
void loop();  // every package's loop()

// Helper shared by packages: profile id of ssh://PROFILE[/path], or "".
String profile_of(const String& target);

}  // namespace vat::registry
