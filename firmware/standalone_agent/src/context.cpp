#include "context.h"

#include <cJSON.h>
#include <time.h>

#include <algorithm>
#include <set>

#include "common.h"
#include "config.h"
#include "storage.h"

namespace vat::context {
namespace {

constexpr char kState[] = "/vat/context/state.json";
constexpr char kOperations[] = "/vat/memory/operations.jsonl";  // archive without a card
constexpr std::size_t kBudgetCard = 48000;
constexpr std::size_t kBudgetFlash = 8000;
// Up to one SSH screenful (4 KB) stays whole in the conversation: a 1.3 KB
// directory listing filed as out#N sent the model paging and rerunning.
constexpr std::size_t kInlineResult = 4200;   // longer results are filed as out#N
constexpr std::size_t kHeadInContext = 2000;  // head of a filed result kept in the conversation
constexpr std::size_t kResultFlash = 600;     // without a card: plain truncation, as before
constexpr std::size_t kPage = 4000;
constexpr uint64_t kOutLimit = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t kCaseNotes = 3000;

struct State {
    long next_out = 1;
    long oldest_out = 1;
    uint64_t out_bytes = 0;
    String current_case;
};
State state;
std::size_t turn_count = 0;
String offered_path;  // complete output fetched by ssh, waiting to be filed
std::size_t offered_bytes = 0;

String today() {
    struct tm now;
    if (!getLocalTime(&now, 0)) return "undated";
    char text[9];
    snprintf(text, sizeof(text), "%04d%02d%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    return text;
}

String clock() {
    struct tm now;
    if (!getLocalTime(&now, 0)) return "--:--";
    char text[6];
    snprintf(text, sizeof(text), "%02d:%02d", now.tm_hour, now.tm_min);
    return text;
}

void load_state() {
    cJSON* json = cJSON_Parse(storage::read_text(kState, 2048).c_str());
    if (json) {
        state.next_out = std::max(1L, json_int(json, "next_out", 1));
        state.oldest_out = std::max(1L, json_int(json, "oldest_out", 1));
        state.out_bytes = static_cast<uint64_t>(json_int(json, "out_bytes", 0));
        state.current_case = json_str(json, "case");
        cJSON_Delete(json);
    }
}

void save_state() {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "next_out", state.next_out);
    cJSON_AddNumberToObject(json, "oldest_out", state.oldest_out);
    cJSON_AddNumberToObject(json, "out_bytes", static_cast<double>(state.out_bytes));
    cJSON_AddStringToObject(json, "case", state.current_case.c_str());
    storage::write_text(kState, json_print(json));
    cJSON_Delete(json);
}

std::size_t count_lines(const char* path) {
    if (!storage::exists(path)) return 0;
    File file = storage::fs().open(path, FILE_READ);
    if (!file) return 0;
    std::size_t lines = 0;
    uint8_t buffer[256];
    int n;
    while ((n = file.read(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) lines += buffer[i] == '\n';
    }
    file.close();
    return lines;
}

String out_path(long id) { return "/vat/out/" + String(id / 1000) + "/" + String(id) + ".txt"; }
String meta_path(long id) { return "/vat/out/" + String(id / 1000) + "/" + String(id) + ".meta.json"; }

// Case names become directory names: one path segment, no tricks.
String case_name(const String& raw) {
    String name = raw;
    name.trim();
    if (name.startsWith("memory://cases/")) name = name.substring(15);
    name.replace("/", "-");
    name.replace("\\", "-");
    while (name.startsWith(".")) name.remove(0, 1);
    return utf8_head(name, 40);
}

String case_dir(const String& name) { return "/vat/cases/" + name; }

std::set<long> referenced_outputs() {
    std::set<long> ids;
    cJSON* cases = cJSON_Parse(storage::list_dir("/vat/cases").c_str());
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, cases) {
        const String text = storage::read_text(case_dir(json_str(entry, "name")) + "/refs.txt", 8192);
        int start = 0;
        while (start < static_cast<int>(text.length())) {
            int end = text.indexOf('\n', start);
            if (end < 0) end = text.length();
            const long id = text.substring(start, end).toInt();
            if (id > 0) ids.insert(id);
            start = end + 1;
        }
    }
    cJSON_Delete(cases);
    return ids;
}

// Oldest outputs go first once the store passes its limit; anything a case
// refers to stays.
void prune_outputs() {
    if (state.out_bytes <= kOutLimit) return;
    const auto keep = referenced_outputs();
    while (state.out_bytes > kOutLimit * 9 / 10 && state.oldest_out < state.next_out) {
        const long id = state.oldest_out++;
        if (keep.count(id)) continue;
        const std::size_t bytes = storage::size(out_path(id));
        storage::remove(out_path(id));
        storage::remove(meta_path(id));
        state.out_bytes = state.out_bytes > bytes ? state.out_bytes - bytes : 0;
    }
}

void remember_refs(const String& name, const String& content) {
    int at = 0;
    while ((at = content.indexOf("out#", at)) >= 0) {
        at += 4;
        const long id = content.substring(at).toInt();
        if (id > 0) storage::append(case_dir(name) + "/refs.txt", String(id) + "\n");
    }
}

bool contains_folded(const String& haystack, const String& needle) {
    String a = haystack;
    String b = needle;
    a.toLowerCase();
    b.toLowerCase();
    return a.indexOf(b) >= 0;
}

// The archive (memory://history) has no other limit: past this total the
// oldest days go.  Checked once per day of writing (and after each boot).
constexpr uint64_t kArchiveCapBytes = 256ULL * 1024 * 1024;
String archive_day;

void cap_archive() {
    std::vector<std::pair<String, uint64_t>> files;  // name, bytes
    uint64_t total = 0;
    cJSON* list = cJSON_Parse(storage::list_dir("/vat/archive").c_str());
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        const String name = json_str(entry, "name");
        if (!name.endsWith(".jsonl")) continue;
        const uint64_t bytes = static_cast<uint64_t>(json_int(entry, "size", 0));
        files.emplace_back(name, bytes);
        total += bytes;
    }
    cJSON_Delete(list);
    std::sort(files.begin(), files.end());  // YYYYMMDD: oldest first
    for (std::size_t i = 0; total > kArchiveCapBytes && i + 1 < files.size(); ++i) {  // never today's file
        storage::remove("/vat/archive/" + files[i].first);
        total -= files[i].second;
        runtime_event("archive_pruned", files[i].first);
    }
}

std::vector<String> archive_files() {  // newest first
    std::vector<String> names;
    cJSON* list = cJSON_Parse(storage::list_dir("/vat/archive").c_str());
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        const String name = json_str(entry, "name");
        if (name.endsWith(".jsonl")) names.push_back(name);
    }
    cJSON_Delete(list);
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) { return a > b; });
    return names;
}

}  // namespace

void begin() {
    load_state();
    if (on_card()) {
        storage::remove("/vat/cache/full.txt");
        for (const char* dir : {"/vat/context", "/vat/out", "/vat/archive", "/vat/cases"}) {
            if (!storage::exists(dir)) storage::fs().mkdir(dir);
        }
    } else if (!storage::exists("/vat/context")) {
        storage::fs().mkdir("/vat/context");
    }
    turn_count = count_lines(kWindow);
}

bool on_card() { return storage::kind() == "tf"; }

bool owns_target(const String& target) {
    for (const char* prefix : {"memory://out", "memory://history", "memory://cases"}) {
        const std::size_t n = strlen(prefix);
        if (target.startsWith(prefix) && (target.length() == n || target[n] == '/' || target[n] == '?')) return true;
    }
    return false;
}
std::size_t budget() { return on_card() ? config::get().context_kb * 1000u : kBudgetFlash; }

// ---- window ------------------------------------------------------------------

bool append_turn(const String& fragment) {
    if (!storage::append(kWindow, fragment + "\n")) return false;
    ++turn_count;
    return true;
}

std::size_t turns() { return turn_count; }
std::size_t window_bytes() { return storage::size(kWindow); }

std::size_t tail_offset(std::size_t max_bytes) {
    const std::size_t size = window_bytes();
    if (size <= max_bytes) return 0;
    const std::size_t want = size - max_bytes;
    File file = storage::fs().open(kWindow, FILE_READ);
    if (!file) return size;
    std::size_t position = 0;
    uint8_t buffer[256];
    int n;
    while ((n = file.read(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i, ++position) {
            if (buffer[i] == '\n' && position + 1 >= want) {
                file.close();
                return position + 1;
            }
        }
    }
    file.close();
    return size;
}

bool drop_before(std::size_t offset) {
    if (offset == 0) return true;
    const String temp = String(kWindow) + ".tmp";
    storage::remove(temp);
    File source = storage::fs().open(kWindow, FILE_READ);
    File target = storage::fs().open(temp, FILE_WRITE);
    storage::changed(temp);  // written through fs(): not "missing" any more
    const bool ok = source && target && source.seek(offset) && storage::copy_stream(source, target);
    if (source) source.close();
    if (target) target.close();
    if (!ok || !storage::rename(temp, kWindow)) {
        storage::remove(temp);
        return false;
    }
    turn_count = count_lines(kWindow);
    return true;
}

bool write_summary(const String& text) {
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "system");
    cJSON_AddStringToObject(message, "content", text.c_str());
    const String json = json_print(message);
    cJSON_Delete(message);
    return !json.isEmpty() && storage::write_text(kSummary, "," + json);
}

void task_begin() { storage::remove(kTask); }
bool task_append(const String& fragment) { return storage::append(kTask, fragment); }
std::size_t task_bytes() { return storage::size(kTask); }

bool commit_task(const String& user_json, const String& answer_json) {
    if (!storage::append(kWindow, "," + user_json)) return false;
    bool ok = true;
    if (storage::exists(kTask)) {
        File source = storage::fs().open(kTask, FILE_READ);
        File target = storage::fs().open(kWindow, FILE_APPEND);
        ok = source && target && storage::copy_stream(source, target);
        if (source) source.close();
        if (target) target.close();
        storage::changed(kWindow);  // appended through fs()
    }
    ok = ok && storage::append(kWindow, "," + answer_json + "\n");
    if (ok) ++turn_count;
    storage::remove(kTask);
    return ok;
}

void clear() {
    storage::remove(kWindow);
    storage::remove(kSummary);
    turn_count = 0;
}

// ---- outputs -------------------------------------------------------------------

void offer_full_output(const String& cache_path, std::size_t bytes) {
    offered_path = cache_path;
    offered_bytes = bytes;
}

String compact_result(const String& label, const String& result) {
    const bool offered = !offered_path.isEmpty();
    if (!on_card()) {
        offered_path = "";
        return utf8_head(result, kResultFlash);
    }
    if (!offered && result.length() <= kInlineResult) return result;
    const long id = state.next_out++;
    std::size_t bytes = result.length();
    bool filed;
    if (offered) {
        filed = storage::rename(offered_path, out_path(id));
        bytes = offered_bytes;
        offered_path = "";
    } else {
        filed = storage::ensure_parent(out_path(id)) && storage::write_text(out_path(id), result);
    }
    if (!filed) {
        --state.next_out;
        return utf8_head(result, kResultFlash);
    }
    cJSON* meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "id", id);
    cJSON_AddStringToObject(meta, "label", utf8_head(label, 160).c_str());
    cJSON_AddNumberToObject(meta, "bytes", bytes);
    cJSON_AddStringToObject(meta, "date", today().c_str());
    cJSON_AddStringToObject(meta, "time", clock().c_str());
    storage::write_text(meta_path(id), json_print(meta));
    cJSON_Delete(meta);
    state.out_bytes += bytes;
    prune_outputs();
    save_state();
    const long pages = (bytes + kPage - 1) / kPage;
    return utf8_head(result, kHeadInContext) + "\n…（完整输出 out#" + String(id) + "，共 " + String(bytes) +
           " 字节 " + String(pages) + " 页；用 read memory://out/" + String(id) +
           "?page=2 看下一页，?grep=关键词 只看匹配行，不要重新执行命令）";
}

String read_output(long id, long page, const String& grep) {
    if (!on_card()) return "没有 TF 卡，完整输出不可用";
    const String path = out_path(id);
    const std::size_t bytes = storage::size(path);
    if (!bytes) return "没有 out#" + String(id) + "（可能已被清理）";
    if (!grep.isEmpty()) {
        File file = storage::fs().open(path, FILE_READ);
        String hits;
        long line_no = 0;
        long matched = 0;
        while (file && file.available()) {
            String line = file.readStringUntil('\n');
            ++line_no;
            if (!contains_folded(line, grep)) continue;
            ++matched;
            if (hits.length() < kPage) hits += String(line_no) + ": " + utf8_head(line, 300) + "\n";
        }
        if (file) file.close();
        if (!matched) return "out#" + String(id) + " 中没有匹配 “" + grep + "” 的行（共 " + String(line_no) + " 行）";
        return "out#" + String(id) + " 中匹配 “" + grep + "” 的行（共 " + String(matched) + " 行，格式 行号: 内容）：\n" +
               hits;
    }
    const long pages = (bytes + kPage - 1) / kPage;
    page = constrain(page, 1L, pages);
    // Pages are byte ranges; widen to whole UTF-8 characters at both ends.
    const String raw = storage::read_range(path, (page - 1) * kPage, kPage + 3);
    std::size_t begin = 0;
    while (begin < raw.length() && (static_cast<uint8_t>(raw[begin]) & 0xC0) == 0x80) ++begin;
    std::size_t end = std::min<std::size_t>(raw.length(), kPage);
    while (end < raw.length() && (static_cast<uint8_t>(raw[end]) & 0xC0) == 0x80) ++end;
    return "out#" + String(id) + " 第 " + String(page) + "/" + String(pages) + " 页：\n" +
           raw.substring(begin, end);
}

String list_outputs() {
    if (!on_card()) return "没有 TF 卡，完整输出不可用";
    String text;
    int shown = 0;
    for (long id = state.next_out - 1; id >= state.oldest_out && shown < 12; --id) {
        cJSON* meta = cJSON_Parse(storage::read_text(meta_path(id), 1024).c_str());
        if (!meta) continue;
        ++shown;
        text += String(shown) + ". out#" + String(id) + "  " + json_str(meta, "date") + " " + json_str(meta, "time") +
                "  " + String(json_int(meta, "bytes", 0)) + " 字节  " + json_str(meta, "label") + "\n";
        cJSON_Delete(meta);
    }
    return text.isEmpty() ? String("还没有保存的完整输出") : "最近的完整输出（新的在前）：\n" + text;
}

// ---- archive -------------------------------------------------------------------

void archive(const String& record_json) {
    if (on_card()) {
        const String day = today();
        if (day != archive_day) {
            archive_day = day;
            cap_archive();
        }
        storage::append_buffered("/vat/archive/" + day + ".jsonl", record_json + "\n", false);
        return;
    }
    storage::append_line(kOperations, record_json);
}

String recent_operations(std::size_t max_bytes) {
    if (!on_card()) return storage::read_tail(kOperations, max_bytes);
    storage::flush();
    const auto files = archive_files();
    return files.empty() ? String() : storage::read_tail("/vat/archive/" + files.front(), max_bytes);
}

String search_history(const String& query) {
    if (!on_card()) return "没有 TF 卡，历史检索不可用";
    String needle = query;
    needle.trim();
    if (needle.isEmpty()) return "请给出要检索的关键词（q=…）";
    storage::flush();  // the archive is written behind
    // Every space-separated word must appear (in any order).
    std::vector<String> words;
    int start = 0;
    while (start < static_cast<int>(needle.length())) {
        int end = needle.indexOf(' ', start);
        if (end < 0) end = needle.length();
        if (end > start) words.push_back(needle.substring(start, end));
        start = end + 1;
    }
    auto matches = [&words](const String& line) {
        for (const auto& word : words) {
            if (!contains_folded(line, word)) return false;
        }
        return true;
    };
    String text;
    int hits = 0;
    for (const auto& name : archive_files()) {
        File file = storage::fs().open("/vat/archive/" + name, FILE_READ);
        std::vector<String> found;  // newest last within a file
        while (file && file.available()) {
            const String line = file.readStringUntil('\n');
            if (matches(line)) found.push_back(line);
        }
        if (file) file.close();
        for (auto it = found.rbegin(); it != found.rend() && hits < 12; ++it) {
            cJSON* record = cJSON_Parse(it->c_str());
            if (!record) continue;
            ++hits;
            String line = String(hits) + ". " + name.substring(0, 8) + " " + json_str(record, "time") + " " +
                          utf8_head(json_str(record, "transcript"), 60);
            const String tool = json_str(record, "tool");
            if (!tool.isEmpty()) {
                line += " → " + tool + " " + json_str(record, "target") + " [" + json_str(record, "decision") + "] " +
                        utf8_head(json_str(record, "result"), 120);
            } else {
                line += " → " + utf8_head(json_str(record, "reply"), 120);
            }
            const long out = json_int(record, "out", 0);
            if (out > 0) line += " (out#" + String(out) + ")";
            text += line + "\n";
            cJSON_Delete(record);
        }
        if (hits >= 12) break;
    }
    return hits ? "历史中匹配 “" + needle + "” 的记录（新的在前）：\n" + text : "历史中没有匹配 “" + needle + "” 的记录";
}

// ---- cases ---------------------------------------------------------------------

String current_case() { return state.current_case; }

String attach_case(const String& raw) {
    if (!on_card()) return "没有 TF 卡，案卷不可用";
    const String name = case_name(raw);
    if (name.isEmpty()) return "请给出案卷名称，例如 memory://cases/内网排查";
    const String notes = case_dir(name) + "/notes.md";
    if (!storage::exists(notes)) storage::write_text(notes, "");
    state.current_case = name;
    save_state();
    return "当前案卷：" + name + "（要点和最近的发现会自动带入之后的对话）";
}

String detach_case(const String& raw) {
    const String name = case_name(raw);
    if (state.current_case.isEmpty()) return "当前没有案卷";
    if (!name.isEmpty() && name != state.current_case) return "当前案卷是 " + state.current_case + "，不是 " + name;
    const String was = state.current_case;
    state.current_case = "";
    save_state();
    return "已关闭案卷：" + was + "（内容仍保留在卡上）";
}

String list_cases() {
    if (!on_card()) return "没有 TF 卡，案卷不可用";
    cJSON* list = cJSON_Parse(storage::list_dir("/vat/cases").c_str());
    String text;
    int n = 0;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "dir"))) continue;
        const String name = json_str(entry, "name");
        const std::size_t findings = count_lines((case_dir(name) + "/findings.jsonl").c_str());
        text += String(++n) + ". " + name + "  " + String(findings) + " 条发现" +
                (name == state.current_case ? "  当前" : "") + "\n";
    }
    cJSON_Delete(list);
    return n ? "案卷：\n" + text : String("还没有案卷");
}

String read_case(const String& raw) {
    if (!on_card()) return "没有 TF 卡，案卷不可用";
    const String name = case_name(raw);
    if (!storage::exists(case_dir(name))) return "没有案卷 " + name;
    String text = "案卷 " + name + "\n要点：\n" + storage::read_tail(case_dir(name) + "/notes.md", kCaseNotes);
    const String findings = storage::read_tail(case_dir(name) + "/findings.jsonl", 3000);
    int n = 0;
    String listed;
    int start = 0;
    while (start < static_cast<int>(findings.length())) {
        int end = findings.indexOf('\n', start);
        if (end < 0) end = findings.length();
        cJSON* record = cJSON_Parse(findings.substring(start, end).c_str());
        start = end + 1;
        if (!record) continue;
        listed += String(++n) + ". " + json_str(record, "date") + " " + json_str(record, "time") + " " +
                  json_str(record, "content") + "\n";
        cJSON_Delete(record);
    }
    return text + "\n发现：\n" + (n ? listed : String("（暂无）\n"));
}

String write_case(const String& raw, const String& content, bool append) {
    if (!on_card()) return "没有 TF 卡，案卷不可用";
    const String name = case_name(raw);
    if (name.isEmpty()) return "请给出案卷名称";
    if (content.isEmpty()) return "内容为空，未写入";
    if (append) {
        cJSON* record = cJSON_CreateObject();
        cJSON_AddStringToObject(record, "date", today().c_str());
        cJSON_AddStringToObject(record, "time", clock().c_str());
        cJSON_AddStringToObject(record, "content", utf8_head(content, 1000).c_str());
        const bool ok = storage::append(case_dir(name) + "/findings.jsonl", json_print(record) + "\n");
        cJSON_Delete(record);
        if (!ok) return "写入案卷失败";
    } else if (!storage::write_text(case_dir(name) + "/notes.md", utf8_head(content, 4000))) {
        return "写入案卷失败";
    }
    remember_refs(name, content);
    return (append ? "已记入案卷 " : "已更新案卷要点 ") + name;
}

String case_fragment() {
    if (state.current_case.isEmpty() || !on_card()) return {};
    const String body = read_case(state.current_case);
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "system");
    cJSON_AddStringToObject(message, "content",
                            ("当前案卷（之后的发现用 write memory://cases/" + state.current_case +
                             " mode=append 记录）：\n" + utf8_head(body, 4000))
                                .c_str());
    const String json = json_print(message);
    cJSON_Delete(message);
    return json.isEmpty() ? String() : "," + json;
}

}  // namespace vat::context
