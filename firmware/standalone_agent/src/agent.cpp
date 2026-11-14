#include "agent.h"

#include <algorithm>
#include <vector>

#include "agent_prompt.h"
#include "common.h"
#include "config.h"
#include "context.h"
#include "net.h"
#include "ops_ssh.h"
#include "policy.h"
#include "registry.h"
#include "storage.h"

namespace vat::agent {
namespace {

constexpr int kMaxDiscoverCalls = 2;
constexpr std::size_t kDiscoverResultBytes = 2600;  // the package list grows with packages
constexpr int kMaxCallsPerRound = 3;
constexpr std::size_t kLegacyHistoryChars = 8000;  // reading the pre-v0.4 history once
constexpr std::size_t kMaxReplyBytes = 1500;

// Each turn is a JSON array of chat messages exactly as sent to the model:
// user text, then either an assistant text reply or an assistant tool_calls
// message followed by one tool result per call.  Keeping real tool messages
// (instead of rendering results as assistant prose) stops the model from
// imitating device output as if it were a reply.
//
// Turns live in the context window on storage (context.h) and are streamed
// into each request from there, so no long-lived heap blocks fragment the TLS
// working set.  This file is the pre-v0.4 history, migrated once at boot.
constexpr char kConversations[] = "/vat/memory/conversations.jsonl";

// A turn must start with the user and every tool message must answer a call
// of the preceding assistant message; anything else would be rejected by the
// API and poison every later request, so it is skipped.
bool valid_turn(const cJSON* messages) {
    if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) == 0) return false;
    if (json_str(cJSON_GetArrayItem(messages, 0), "role") != "user") return false;
    std::vector<String> open_ids;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, messages) {
        const String role = json_str(item, "role");
        if (role == "assistant") {
            if (!open_ids.empty()) return false;
            const cJSON* call = nullptr;
            cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(item, "tool_calls")) {
                const String id = json_str(call, "id");
                if (id.isEmpty() || !json_obj(call, "function")) return false;
                open_ids.push_back(id);
            }
        } else if (role == "tool") {
            const String id = json_str(item, "tool_call_id");
            auto it = std::find(open_ids.begin(), open_ids.end(), id);
            if (id.isEmpty() || it == open_ids.end() || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "content"))) {
                return false;
            }
            open_ids.erase(it);
        } else if (role != "user") {
            return false;
        }
    }
    return open_ids.empty();
}

std::vector<String> load_legacy_turns() {
    std::vector<String> turns;
    const String tail = storage::read_tail(kConversations, kLegacyHistoryChars + 4000);
    int start = 0;
    while (start < static_cast<int>(tail.length())) {
        int end = tail.indexOf('\n', start);
        if (end < 0) end = tail.length();
        cJSON* record = cJSON_Parse(tail.substring(start, end).c_str());
        start = end + 1;
        if (!record) continue;
        cJSON* messages = cJSON_GetObjectItemCaseSensitive(record, "messages");
        if (valid_turn(messages)) turns.push_back(json_print(messages));
        cJSON_Delete(record);
    }
    std::size_t total = 0;
    std::size_t keep = 0;
    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
        if (total + it->length() > kLegacyHistoryChars) break;
        total += it->length();
        ++keep;
    }
    turns.erase(turns.begin(), turns.end() - keep);
    return turns;
}

// "[{a},{b}]" -> ",{a},{b}": the form the window stores and streams.
String fragment_of(const String& array_json) {
    return array_json.length() > 2 ? "," + array_json.substring(1, array_json.length() - 1) : String();
}

void migrate_legacy_history() {
    if (storage::exists(context::kWindow) || !storage::exists(kConversations)) return;
    std::size_t moved = 0;
    for (const auto& turn : load_legacy_turns()) moved += context::append_turn(fragment_of(turn));
    storage::remove(kConversations);
    storage::remove("/vat/memory/conversations.1.jsonl");
    runtime_event("history_migrated", String(moved) + " turns");
}

cJSON* chat_message(const char* role, const String& content) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "role", role);
    cJSON_AddStringToObject(item, "content", content.c_str());
    return item;
}

cJSON* tool_call(const String& id, const String& name, const String& arguments) {
    cJSON* call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "id", id.c_str());
    cJSON_AddStringToObject(call, "type", "function");
    cJSON* function = cJSON_AddObjectToObject(call, "function");
    cJSON_AddStringToObject(function, "name", name.c_str());
    cJSON_AddStringToObject(function, "arguments", arguments.c_str());
    return call;
}

cJSON* tool_result(const String& id, const String& content) {
    cJSON* item = chat_message("tool", content);
    cJSON_AddStringToObject(item, "tool_call_id", id.c_str());
    return item;
}

String clock_text() {
    struct tm now;
    if (!getLocalTime(&now, 0)) return "--:--";
    char text[6];
    snprintf(text, sizeof(text), "%02d:%02d", now.tm_hour, now.tm_min);
    return text;
}

// One archive line per executed step (and one per final reply), searchable
// later through memory://history.  `out` is the filed output, if any.
void record_operation(const String& transcript, const Call& call, long out) {
    cJSON* record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "time", clock_text().c_str());
    cJSON_AddStringToObject(record, "transcript", utf8_head(transcript, 300).c_str());
    cJSON_AddStringToObject(record, "tool", call.name.c_str());
    cJSON* arguments = cJSON_Parse(call.arguments.c_str());
    cJSON_AddStringToObject(record, "target", json_str(arguments, "target").c_str());
    cJSON_Delete(arguments);
    cJSON_AddRawToObject(record, "arguments", call.arguments.c_str());
    cJSON_AddStringToObject(record, "decision", call.decision.c_str());
    cJSON_AddStringToObject(record, "result", utf8_head(call.result, 400).c_str());
    if (out > 0) cJSON_AddNumberToObject(record, "out", out);
    context::archive(json_print(record));
    cJSON_Delete(record);
}

void record_reply(const String& transcript, const String& reply) {
    cJSON* record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "time", clock_text().c_str());
    cJSON_AddStringToObject(record, "transcript", utf8_head(transcript, 300).c_str());
    cJSON_AddStringToObject(record, "reply", utf8_head(reply, 400).c_str());
    context::archive(json_print(record));
    cJSON_Delete(record);
}

String profiles_message() {
    const String names = config::profile_names();
    return names.isEmpty() ? String("当前没有可见的 SSH profile。")
                           : "当前可见 SSH profile（只含名称）: " + names + "。";
}

String to_json(cJSON* item) {
    const String json = json_print(item);
    cJSON_Delete(item);
    return json;
}

String summarize_call(const String& tool, const cJSON* arguments) {
    const String custom = registry::confirm_summary(tool, arguments);
    if (!custom.isEmpty()) return utf8_head(custom, 300);
    String line = tool;
    const String target = json_str(arguments, "target");
    if (!target.isEmpty()) line += " " + target;
    if (tool == "exec") {
        line += " $ " + json_str(arguments, "program");
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(arguments, "args");
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, args) {
            if (cJSON_IsString(item)) line += String(" ") + item->valuestring;
        }
    } else if (tool == "write") {
        line += " [" + json_str(arguments, "mode", "overwrite") + "] " + utf8_head(json_str(arguments, "content"), 60);
    } else if (tool == "session") {
        line += " action=" + json_str(arguments, "action");
        const String mode = json_str(arguments, "policy_mode");
        if (!mode.isEmpty()) line += " mode=" + mode;
    }
    return utf8_head(line, 200);
}

}  // namespace

void begin() {
    migrate_legacy_history();
    runtime_event("history_restored", String(context::turns()) + " turns " + String(context::window_bytes()) +
                                          " bytes budget=" + String(context::budget()));
}

String discover_result(const cJSON* arguments) {
    return utf8_head(registry::discover(arguments), kDiscoverResultBytes);
}

void clear_history() { context::clear(); }

std::size_t history_messages() { return context::turns(); }

namespace {

constexpr int kMaxSteps = 10;  // tool rounds per task: write, run, fix and run again fit (6 did not)

// Sent with the last request, where tools are off.  Without it the model often
// answers with the text of one more tool call instead of saying where it got to.
static constexpr char kLastRound[] =
    ",{\"role\":\"system\",\"content\":\"工具轮数已经用完，不能再调用工具。现在只用文字回复用户："
    "已确认做到的、还没确认的、没做完的各是什么，以及查到的原因。不要输出工具调用。\"}";

struct QueuedCall {
    String id;
    String name;
    String arguments;  // compact JSON
    bool approved = false;
    bool announced = false;  // its 'C' step was sent (a paused call comes round again)
};

// One task = one utterance.  Everything needed to continue after a pause
// lives here; it is small (tool results are capped at 600 bytes each).
struct Task {
    bool active = false;
    bool approve_all = false;
    String transcript;
    std::vector<QueuedCall> queue;  // calls of the round not yet executed
    std::vector<Call> calls;
    int rounds = 0;
    int discover = 0;
    int steps = 0;
    unsigned long started = 0;
    bool unattended = false;     // refuse calls that need confirmation or a secret
    unsigned long model_ms = 0;  // time in model requests: the task's budget
    unsigned long tool_ms = 0;   // time running tools: not budgeted (each has its own timeout)
    bool broken = false;      // a message failed to serialise (low memory)
    String last_error;        // the latest ERROR a tool returned
    String last_error_call;   // the call (name + arguments) that returned it
    bool stuck = false;       // the same call failed the same way twice: stop retrying
};
Task task;

// A task's strings, freed: `task = Task{}` alone keeps each one's buffer (an
// Arduino String assigned empty keeps its capacity), so the longest request and
// error of the day stayed allocated between tasks.
void reset_task() {
    release(task.transcript);
    release(task.last_error);
    release(task.last_error_call);
    for (auto& queued : task.queue) release(queued.arguments);
    task = Task{};
}

// Serialises one message of the task; a failed print (cJSON is silent about
// allocation failures) marks the task so its turn is stored in short form.
String piece(cJSON* item) {
    const String json = to_json(item);
    if (json.isEmpty()) task.broken = true;
    return json;
}

String request_head(const String& tool_choice, int max_tokens = 0, bool reasoning = false) {
    const auto& cfg = config::get();
    if (max_tokens <= 0) max_tokens = static_cast<int>(cfg.agent_max_tokens);
    return "{\"model\":" + to_json(cJSON_CreateString(cfg.agent_model.c_str())) +
           ",\"temperature\":0,\"max_tokens\":" + String(max_tokens) + ",\"tool_choice\":\"" + tool_choice +
           "\",\"reasoning_effort\":\"" + (reasoning ? "low" : "none") + "\",\"messages\":[{\"role\":\"system\",\"content\":";
}

// Asks the model with everything so far.  Returns the assistant message.
cJSON* ask_model(const String& tool_choice, String& error, long budget_ms) {
    const auto& cfg = config::get();
    const unsigned long began = millis();
    const String head = request_head(tool_choice, 0, cfg.reasoning);
    const String profiles = "},{\"role\":\"system\",\"content\":" +
                            to_json(cJSON_CreateString(profiles_message().c_str())) + "}";
    const String case_notes = context::case_fragment();
    // Summary and window stream from storage.  The window is normally within
    // budget (maintain() compacts it after each task); if compaction failed,
    // send only its newest part rather than an oversized request.
    const std::size_t summary_bytes = storage::size(context::kSummary);
    const std::size_t window_bytes = context::window_bytes();
    const std::size_t window_from = context::tail_offset(context::budget() * 3 / 2);
    const String user_json = to_json(chat_message("user", task.transcript));
    if (user_json.isEmpty() || user_json == "null") {  // out of memory: never send a broken request
        error = "设备内存不足，这一轮没有发出，请重试";
        return nullptr;
    }
    const String user = "," + user_json;
    static constexpr char kTail[] = "],\"tools\":";
    // System prompt: core rules + each package's usage, all streamed from flash.
    std::vector<net::Segment> parts = {
        {head.c_str(), head.length()},
        {"\"", 1},
        {kCorePrompt, sizeof(kCorePrompt) - 1},
    };
    for (const char* usage : registry::usages()) parts.push_back({usage, strlen(usage)});
    parts.push_back({kClosingPrompt, sizeof(kClosingPrompt) - 1});
    if (!cfg.agent_persona.isEmpty()) parts.push_back({cfg.agent_persona.c_str(), cfg.agent_persona.length()});
    parts.insert(parts.end(), {
        {"\"", 1},
        {profiles.c_str(), profiles.length()},
        {case_notes.c_str(), case_notes.length()},
        {nullptr, summary_bytes, context::kSummary, 0},
        {nullptr, window_bytes - window_from, context::kWindow, window_from},
        {user.c_str(), user.length()},
        {nullptr, context::task_bytes(), context::kTask, 0},
        {kLastRound, tool_choice == "none" ? sizeof(kLastRound) - 1 : 0},
        {kTail, sizeof(kTail) - 1},
        {kToolsJson, sizeof(kToolsJson) - 1},
        {"}", 1},
    });
    ssh::release();  // never hold an SSH session across TLS
    cJSON* message = nullptr;
    // A body that does not parse (cut short on the way: seen once, 4 KB of an 8 KB
    // reply) is asked for again: nothing has run yet, so the request is safe to repeat.
    for (int attempt = 1; attempt <= 2 && !message; ++attempt) {
        const long left = budget_ms - static_cast<long>(millis() - began);
        if (attempt > 1 && left <= 1000) break;
        auto response = net::post_segments(cfg.agent_url, cfg.deepseek_key, parts,
                                           std::min<long>(cfg.agent_round_timeout_ms, attempt > 1 ? left : budget_ms));
        if (!response.ok()) {
            error = response.status == -2 ? String("已取消")
                                          : "Agent HTTP " + String(response.status) + " " +
                                                utf8_head(response.error.isEmpty() ? response.body : response.error, 160);
            return nullptr;
        }
        const std::size_t bytes = response.body.length();
        cJSON* decoded = cJSON_Parse(response.body.c_str());
        response.body = String();  // parsed: a round that writes a program is several KB, twice over until here
        const cJSON* choices = decoded ? cJSON_GetObjectItemCaseSensitive(decoded, "choices") : nullptr;
        const cJSON* choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        // Detached, not copied: a round that writes a program carries its whole source here.
        message = cJSON_IsObject(choice) ? cJSON_DetachItemFromObjectCaseSensitive(const_cast<cJSON*>(choice), "message")
                                         : nullptr;
        if (!cJSON_IsObject(message)) {
            cJSON_Delete(message);
            message = nullptr;
            runtime_event("agent_bad_response", String(decoded ? "no message" : "not JSON") + " bytes=" + String(bytes) +
                                                    " attempt=" + String(attempt));
        }
        cJSON_Delete(decoded);
    }
    if (!message) error = "Agent 响应没有 assistant message";
    return message;
}

String joined_results() {
    String detail;
    for (const auto& call : task.calls) {
        if (call.decision == "discover") continue;
        if (!detail.isEmpty()) detail += "\n";
        detail += call.result;
    }
    return utf8_head(detail, kMaxReplyBytes);
}

// Steps of the task go to storage as they happen (context::kTask).
void task_message(cJSON* item) {
    String json = piece(item);
    if (json.isEmpty() && json_str(item, "role") == "tool") {
        // Printing a long result failed (no memory): the model must still get an
        // answer for this call, or the next request is refused (HTTP 400: an
        // assistant message with tool_calls must be followed by tool messages).
        cJSON* small = tool_result(json_str(item, "tool_call_id"), "ERROR: 内存不足，这一步的结果没能保存；换个更小的请求再试");
        json = piece(small);
        cJSON_Delete(small);
    }
    // The separator on its own: "," + json would be one more copy of a long message.
    if (json.isEmpty() || !context::task_append(",") || !context::task_append(json)) task.broken = true;
}

Outcome finish(const String& reply) {
    Outcome outcome;
    outcome.ok = true;
    outcome.reply = utf8_head(reply.isEmpty() ? String("已完成。") : reply, kMaxReplyBytes);
    outcome.detail = joined_results();
    outcome.calls = task.calls;
    outcome.rounds = task.rounds;
    outcome.steps = task.steps;
    outcome.elapsed_ms = millis() - task.started;
    // The whole exchange goes into the window (tool results are already
    // compacted to their head plus out#N).  Without a card the window is
    // small, so long exchanges keep only the request and the answer, as do
    // turns whose messages failed to serialise.
    const String user = piece(chat_message("user", task.transcript));
    const String answer = piece(chat_message("assistant", utf8_head(outcome.reply, 600)));
    const bool short_form = task.broken || (!context::on_card() && context::task_bytes() > 4000);
    if (!user.isEmpty() && !answer.isEmpty()) {
        const bool ok = short_form ? context::append_turn("," + user + "," + answer)
                                   : context::commit_task(user, answer);
        if (!ok) runtime_event("history_skip", "window append failed");
    }
    context::task_begin();
    record_reply(task.transcript, outcome.reply);
    task.active = false;
    runtime_event("agent_done", String(outcome.elapsed_ms) + "ms model=" + task.model_ms + "ms tools=" +
                                    task.tool_ms + "ms steps=" + outcome.steps + " calls=" + task.calls.size() + " " +
                                    utf8_head(outcome.reply, 120));
    reset_task();  // over: nothing of it is needed until the next one
    return outcome;
}

Outcome fail(const String& error) {
    Outcome outcome;
    outcome.error = error;
    outcome.calls = task.calls;
    outcome.detail = joined_results();
    outcome.rounds = task.rounds;
    outcome.steps = task.steps;
    outcome.elapsed_ms = millis() - task.started;
    task.active = false;
    runtime_event("agent_error", String(outcome.elapsed_ms) + "ms model=" + task.model_ms + "ms tools=" +
                                     task.tool_ms + "ms " + utf8_head(error, 160));
    reset_task();  // over: nothing of it is needed until the next one
    return outcome;
}

Outcome pause_for(const QueuedCall& call, const String& reason, const String& secret_prompt) {
    Outcome outcome;
    outcome.ok = true;
    outcome.paused = true;
    cJSON* arguments = cJSON_Parse(call.arguments.c_str());
    Pending pending{call.name, call.arguments, reason, summarize_call(call.name, arguments), secret_prompt,
                    task.steps};
    cJSON_Delete(arguments);
    outcome.pending.push_back(pending);
    outcome.reply = secret_prompt.isEmpty() ? "需要确认：" + pending.summary : secret_prompt;
    outcome.detail = joined_results();
    outcome.calls = task.calls;
    // The waiting step belongs in the record too (not executed yet).
    outcome.calls.push_back({call.name, call.arguments,
                             secret_prompt.isEmpty() ? "需要确认（尚未执行）：" + reason : String("等待输入密钥（尚未执行）"),
                             secret_prompt.isEmpty() ? "ask" : "secret"});
    outcome.steps = task.steps;
    outcome.elapsed_ms = millis() - task.started;
    runtime_event("agent_pause", String(secret_prompt.isEmpty() ? "confirm " : "secret ") + pending.summary);
    return outcome;
}

bool failed_result(const String& result) { return registry::failed(result); }

// What to do about a failure: every error ends with the next step, so the
// model does not repeat the call or give up where one more read would do.
String hint_for(const String& text) {
    const int uses = text.indexOf("-- @uses ");
    if (uses >= 0) {
        int end = text.indexOf(' ', text.indexOf(' ', uses + 9) + 1);
        const int stop = text.indexOf("，", uses);
        if (end < 0 || (stop >= 0 && stop < end)) end = stop;
        return "改程序：在开头加一行 " + text.substring(uses, end < 0 ? text.length() : end) +
               "，再 write prog://名字 保存（保存时用户确认一次），然后再运行。";
    }
    if (text.startsWith("没有能力包可处理")) {
        return "目标写错了或设备没有这项能力：用 discover（scope=resources，query=关键词）查可用的资源；查不到就告诉用户设备不支持。";
    }
    if (text.indexOf("程序太大") >= 0) return "先把写法收紧再保存；还放不下，再把不需要共用数据的部分拆成另一个程序（每次调用另一个程序都是重新开始，它不记得上一次的数据）。";
    if (text.indexOf("语法错误") >= 0) return "按报错里的行号改源码，再保存。";
    if (text.indexOf("的原文在程序里") >= 0) return "先 read 程序里要改的那一段原文，再照原文重写补丁。";
    const int name_from = text.indexOf("程序「");
    const int name_to = text.indexOf("」失败：");
    if (name_from >= 0 && name_to > name_from) {
        const String name = text.substring(name_from + strlen("程序「"), name_to);
        const String rest = text.substring(name_to + strlen("」失败："));
        if (rest.startsWith(name + ":")) {
            return "read prog://" + name + " 看第 " + String(rest.substring(name.length() + 1).toInt()) +
                   " 行附近，改好后保存并重新运行。";
        }
    }
    if (text.startsWith("SSH 连接失败")) return "主机连不上：不要反复重试，告诉用户检查主机和网络。";
    if (text.startsWith("SSH 失败")) {
        return "命令在远端执行了但报告失败（退出码不是 0）：看上面的错误输出，路径或参数写错就改了再执行，不要原样重试。";
    }
    if (text.indexOf("缺少参数") >= 0) return "运行时带上参数：exec prog://名字?参数=值。";
    if (text.indexOf("没有程序「") >= 0) return "read prog://list 看有哪些程序，名字要完全一致。";
    return "读懂上面的原因再决定：改参数重试一次，或换个方法；同样的错误不要重复两次。";
}

// A tool result as the model reads it: the first line says whether it failed
// and how long it took; a failure ends with what to do next.  `ms` < 0: the
// call did not run (rejected, unattended), the text already says why.
// `kept` is what stays in the conversation (the result, or its head and out#N).
String presented(const String& result, const String& kept, long ms) {
    if (ms < 0) return kept;
    const bool failed = failed_result(result);
    String out = String(failed ? "失败" : "已执行") + " · " + String(ms) + " ms\n" + kept;
    if (failed) out += "\n怎么改：" + hint_for(result);
    return out;
}

// What the task keeps of a call until it ends (the timeline, the job record):
// not the whole source of a program it wrote, nor a long result.  Both are in
// storage already; in memory they were several copies of several KB each.
// The first `max` bytes of `text`, cut at a UTF-8 character boundary, without
// copying the rest of it.
String head_of(const char* text, std::size_t max) {
    std::size_t n = strnlen(text, max + 1);
    if (n > max) {
        n = max;
        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
    }
    String out;
    out.reserve(n);
    out.concat(text, n);
    return out;
}

// `arguments` printed with a long content cut to its head (the tree is left as it was).
String slim_tree(cJSON* arguments) {
    cJSON* content = cJSON_GetObjectItemCaseSensitive(arguments, "content");
    const std::size_t bytes = cJSON_IsString(content) ? strlen(content->valuestring) : 0;
    if (bytes <= 400) return json_print(arguments);
    cJSON* full = cJSON_DetachItemViaPointer(arguments, content);
    cJSON_AddStringToObject(arguments, "content",
                            (head_of(full->valuestring, 300) + "…（共 " + String(static_cast<unsigned>(bytes)) + " 字节）").c_str());
    const String out = json_print(arguments);
    cJSON_DeleteItemFromObjectCaseSensitive(arguments, "content");
    cJSON_AddItemToObject(arguments, "content", full);
    return out;
}

String slim_arguments(const String& arguments) {
    if (arguments.length() <= 800) return arguments;
    cJSON* parsed = cJSON_Parse(arguments.c_str());
    const String content = json_str(parsed, "content");
    if (content.length() <= 400) {
        cJSON_Delete(parsed);
        return arguments;
    }
    cJSON_ReplaceItemInObjectCaseSensitive(
        parsed, "content",
        cJSON_CreateString((utf8_head(content, 300) + "…（共 " + String(content.length()) + " 字节）").c_str()));
    return to_json(parsed);
}

void stage(const Progress& progress, const String& name) {
    if (progress.stage) progress.stage(name);
}

void step(const Progress& progress, char kind, String text) {
    text.trim();
    if (progress.step && !text.isEmpty()) progress.step(kind, text);
}

void record_result(const QueuedCall& call, const String& decision, const String& result, const Progress& progress,
                   long ms = -1) {
    if (decision != "discover") {
        step(progress, decision == "deny" || decision == "rejected" || decision == "secret_cancelled" ? 'X' : 'R',
             utf8_head(result, 240));
    }
    Call record{call.name, slim_arguments(call.arguments), result, decision};
    String in_context = result;
    long out = 0;
    if (decision != "discover") {
        cJSON* arguments = cJSON_Parse(call.arguments.c_str());
        in_context = presented(result, context::compact_result(summarize_call(call.name, arguments), result), ms);
        cJSON_Delete(arguments);
        const int ref = in_context.lastIndexOf("完整输出 out#");
        if (ref >= 0) out = in_context.substring(ref + String("完整输出 out#").length()).toInt();
        record_operation(task.transcript, record, out);
    }
    record.result = utf8_head(record.result, 1500);
    task.calls.push_back(std::move(record));
    task_message(tool_result(call.id, utf8_sanitize(in_context)));
    if (registry::failed(result)) {
        const String key = call.name + call.arguments;
        if (key == task.last_error_call && result == task.last_error) task.stuck = true;
        task.last_error = result;
        task.last_error_call = key;
    } else {
        task.last_error_call = "";
    }
}

// What went wrong, for a reply that has nothing better to say.
String failure_reply(const String& why) {
    String error = task.last_error;
    if (error.startsWith("ERROR:")) error = error.substring(6);
    error.trim();
    return "这次没做成：" + (error.isEmpty() ? why : error + "（" + why + "）");
}

// DeepSeek sometimes writes a tool call into the text as markup
// (<｜｜DSML｜｜ calls>…) when it may not call tools: never show that.
String clean_reply(String content) {
    const int mark = content.indexOf("DSML");
    if (mark < 0) return content;
    const int open = content.lastIndexOf('<', mark);
    content = content.substring(0, open >= 0 ? open : mark);
    content.trim();
    return content.isEmpty() ? failure_reply("步数用完了还想继续调用工具") : content;
}

// Runs queued calls of the current round.  Returns true when it paused.
bool drain_queue(Outcome& paused, const String* secret, const Progress& progress) {
    while (!task.queue.empty()) {
        if (cancel::requested()) return false;
        QueuedCall& call = task.queue.front();
        cJSON* arguments = cJSON_Parse(call.arguments.c_str());
        if (!arguments) arguments = cJSON_CreateObject();
        if (call.name == "discover") {
            ++task.discover;
            stage(progress, "DISCOVER");
            const String result = task.discover > kMaxDiscoverCalls ? String("{\"error\":\"discover 次数已达上限\"}")
                                                                    : discover_result(arguments);
            record_result(call, "discover", result, progress);
        } else {
            if (!call.announced) {
                call.announced = true;
                step(progress, 'C', describe_call(call.name, call.arguments));
            }
            const auto verdict = policy::evaluate(call.name, arguments);
            if (verdict.decision == policy::Decision::Deny) {
                record_result(call, "deny", "已拒绝：" + verdict.reason, progress);
            } else if (verdict.decision == policy::Decision::Ask && !call.approved && !task.approve_all &&
                       task.unattended) {
                record_result(call, "rejected", "无人值守，不执行需要确认的操作：" + verdict.reason, progress);
            } else if (verdict.decision == policy::Decision::Ask && !call.approved && !task.approve_all) {
                cJSON_Delete(arguments);
                step(progress, 'W', "等待确认：" + verdict.reason);
                paused = pause_for(call, verdict.reason, "");
                return true;
            } else if (task.unattended && !registry::needs_secret(call.name, arguments).isEmpty()) {
                record_result(call, "rejected", "无人值守，不能输入密码", progress);
            } else {
                const String prompt = registry::needs_secret(call.name, arguments);
                if (!prompt.isEmpty() && !secret) {
                    if (verdict.decision == policy::Decision::Ask) call.approved = true;
                    cJSON_Delete(arguments);
                    step(progress, 'W', prompt);
                    paused = pause_for(call, "", prompt);
                    return true;
                }
                stage(progress, "RUN " + call.name);
                // From here the parsed arguments do the work; the queue's copy of a
                // program's source would only sit beside it while it compiles.
                if (call.arguments.length() > 800) call.arguments = slim_tree(arguments);
                const bool approved = call.approved || task.approve_all || verdict.decision == policy::Decision::Ask;
                const unsigned long began = millis();
                const String result = registry::execute(call.name, arguments, approved, secret ? *secret : String());
                const String decision = verdict.decision == policy::Decision::Allow ? "allow"
                                        : task.approve_all && !call.approved ? "approved_all" : "approved";
                record_result(call, decision, result, progress, static_cast<long>(millis() - began));
                secret = nullptr;  // a secret is used for exactly one call
            }
        }
        cJSON_Delete(arguments);
        task.queue.erase(task.queue.begin());
    }
    return false;
}

// Model rounds until a text answer, a pause, or the step limit.
Outcome advance(const String* secret, const Progress& progress) {
    const auto& cfg = config::get();
    while (true) {
        Outcome paused;
        const unsigned long tools_from = millis();
        const bool stop = drain_queue(paused, secret, progress);
        task.tool_ms += millis() - tools_from;
        if (stop) return paused;
        if (task.stuck) return finish(failure_reply("同样的调用连续两次出错，已停止重试"));
        secret = nullptr;
        if (cancel::requested()) return fail("已取消");
        // Only model time is budgeted; tools, and waiting for the user, are not.
        const long budget = static_cast<long>(cfg.agent_total_timeout_ms) - static_cast<long>(task.model_ms);
        if (budget <= 1000) return fail("理解超时：模型累计用了 " + String(task.model_ms / 1000) + " 秒");
        const bool last = task.steps >= kMaxSteps;
        stage(progress, last ? String("SUMMARY") : "THINK " + String(task.steps + 1));
        String error;
        const unsigned long asked = millis();
        cJSON* message = ask_model(last ? "none" : "auto", error, budget);
        task.model_ms += millis() - asked;
        if (!message) return fail(error);
        ++task.rounds;
        step(progress, 'T', utf8_head(json_str(message, "reasoning_content"), 240));
        cJSON* tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
        const int count = cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0;
        if (count == 0 || last) {
            String content = json_str(message, "content");
            content.trim();
            cJSON_Delete(message);
            return finish(clean_reply(content));
        }
        // Queue this round's calls (at most three).
        cJSON* assistant = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant, "role", "assistant");
        const String content = json_str(message, "content");
        step(progress, 'S', utf8_head(content, 240));
        if (content.isEmpty()) cJSON_AddNullToObject(assistant, "content");
        else cJSON_AddStringToObject(assistant, "content", content.c_str());
        // Thinking mode refuses a request whose calling messages of this turn come
        // back without reasoning_content (HTTP 400), also one made before the
        // setting was turned on: an empty one will do there.
        const String thought = json_str(message, "reasoning_content");
        if (cfg.reasoning || !thought.isEmpty()) {
            cJSON_AddStringToObject(assistant, "reasoning_content", utf8_head(thought, 1500).c_str());
        }
        cJSON* kept = cJSON_AddArrayToObject(assistant, "tool_calls");
        bool counts_as_step = false;
        // A call that writes a program carries its whole source: it is parsed once to
        // check it, kept once for the queue, and the reply's own item moves into
        // the history (it used to be copied there, and the queue held a reprint).
        for (int i = 0; i < count && i < kMaxCallsPerRound; ++i) {
            cJSON* raw = cJSON_DetachItemFromArray(tool_calls, 0);
            if (!raw) break;
            cJSON* function = cJSON_GetObjectItemCaseSensitive(raw, "function");
            if (!cJSON_IsObject(function)) {
                cJSON_Delete(raw);
                continue;
            }
            QueuedCall call;
            call.id = json_str(raw, "id", "call-" + String(millis()) + "-" + String(i));
            call.name = json_str(function, "name", "unknown");
            cJSON* text = cJSON_GetObjectItemCaseSensitive(function, "arguments");
            cJSON* parsed = cJSON_IsString(text) ? cJSON_Parse(text->valuestring) : nullptr;
            const bool valid = cJSON_IsObject(parsed);
            cJSON_Delete(parsed);
            if (valid) {
                call.arguments = text->valuestring;
            } else {  // what the model sent is not an object: both sides see {}
                call.arguments = "{}";
                cJSON_ReplaceItemInObjectCaseSensitive(function, "arguments", cJSON_CreateString("{}"));
            }
            cJSON_DeleteItemFromObjectCaseSensitive(raw, "index");  // the stream position, not part of a message
            cJSON_ReplaceItemInObjectCaseSensitive(raw, "id", cJSON_CreateString(call.id.c_str()));
            if (!cJSON_GetObjectItemCaseSensitive(raw, "type")) cJSON_AddStringToObject(raw, "type", "function");
            cJSON_AddItemToArray(kept, raw);
            counts_as_step = counts_as_step || call.name != "discover";
            task.queue.push_back(std::move(call));
        }
        cJSON_Delete(message);
        task_message(assistant);
        if (counts_as_step) ++task.steps;
        stage(progress, "STEP " + String(task.steps));
    }
}

}  // namespace

Outcome run(const String& transcript, const Progress& progress, bool unattended) {
    reset_task();
    context::task_begin();
    task.active = true;
    task.transcript = transcript;
    task.unattended = unattended;
    task.started = millis();
    runtime_event("agent_start", utf8_head(transcript, 120));
    return advance(nullptr, progress);
}

Outcome resume(Answer answer, const String& secret, const Progress& progress) {
    if (!task.active || task.queue.empty()) return fail("没有等待中的任务");
    task.started = millis();
    QueuedCall& call = task.queue.front();
    if (answer == Answer::Reject) {
        record_result(call, "rejected", "用户在设备上拒绝了这一步，未执行。", progress);
        task.queue.erase(task.queue.begin());
        return advance(nullptr, progress);
    }
    if (answer == Answer::Secret) {
        if (secret.isEmpty()) {
            record_result(call, "secret_cancelled", "用户取消了密码输入，未执行。", progress);
            task.queue.erase(task.queue.begin());
            return advance(nullptr, progress);
        }
        return advance(&secret, progress);
    }
    call.approved = true;
    if (answer == Answer::ApproveAll) task.approve_all = true;
    return advance(nullptr, progress);
}

String describe_call(const String& name, const String& arguments) {
    cJSON* args = cJSON_Parse(arguments.c_str());
    String what = name;
    const String target = json_str(args, "target");
    if (!target.isEmpty()) what += " " + target;
    if (name == "exec") {
        what += " $ " + json_str(args, "program");
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(args, "args")) {
            if (cJSON_IsString(item)) what += String(" ") + item->valuestring;
        }
    } else if (name == "session") {
        what += " " + json_str(args, "action");
    }
    cJSON_Delete(args);
    return utf8_head(what, 160);
}

bool paused() { return task.active && !task.queue.empty(); }
bool active() { return task.active; }

void maintain() {
    if (task.active) return;
    const std::size_t size = context::window_bytes();
    if (size <= context::budget()) return;
    // Keep the newest half of the budget verbatim; summarise everything older.
    const std::size_t cut = context::tail_offset(context::budget() / 2);
    if (cut == 0 || cut >= size) return;
    static constexpr char kInstruction[] =
        "\"你是操作记录员。下面是之前的摘要（如有）和较早的对话，请把它们合并成新的摘要："
        "保留主机、路径、命令、关键结果数字、发现、未完成事项和 out#编号，删掉寒暄和重复；"
        "按主题分条，中文，不超过 1200 字，不要编造。\"";
    static constexpr char kAsk[] = ",{\"role\":\"user\",\"content\":\"请输出合并后的摘要。\"}";
    static constexpr char kTail[] = "],\"tools\":";
    const String head = request_head("none", 1500);
    const std::size_t summary_bytes = storage::size(context::kSummary);
    const std::vector<net::Segment> parts = {
        {head.c_str(), head.length()},
        {kInstruction, sizeof(kInstruction) - 1},
        {"}", 1},
        {nullptr, summary_bytes, context::kSummary, 0},
        {nullptr, cut, context::kWindow, 0},
        {kAsk, sizeof(kAsk) - 1},
        {kTail, sizeof(kTail) - 1},
        {kToolsJson, sizeof(kToolsJson) - 1},
        {"}", 1},
    };
    const unsigned long started = millis();
    ssh::release();
    const auto& cfg = config::get();
    const auto response = net::post_segments(cfg.agent_url, cfg.deepseek_key, parts, cfg.agent_round_timeout_ms);
    String summary;
    if (response.ok()) {
        cJSON* decoded = cJSON_Parse(response.body.c_str());
        const cJSON* choices = decoded ? cJSON_GetObjectItemCaseSensitive(decoded, "choices") : nullptr;
        summary = json_str(json_obj(cJSON_GetArrayItem(choices, 0), "message"), "content");
        cJSON_Delete(decoded);
        summary.trim();
    }
    if (summary.isEmpty()) {
        runtime_event("context_compact_fail", "HTTP " + String(response.status) + " " + utf8_head(response.error, 80));
        return;
    }
    const bool ok = context::write_summary(
                        "此前对话的摘要（更早的细节用 read memory://history?q=关键词 检索，完整输出用 out#编号 查看）：\n" +
                        utf8_head(summary, 4000)) &&
                    context::drop_before(cut);
    runtime_event(ok ? "context_compact" : "context_compact_fail",
                  String(size) + " -> " + String(context::window_bytes()) + " bytes, summary " +
                      String(summary.length()) + " bytes, " + String(millis() - started) + "ms");
}

void abort_task() {
    if (!task.active) return;
    runtime_event("agent_abort", utf8_head(task.transcript, 80));
    reset_task();
    context::task_begin();
}

}  // namespace vat::agent
