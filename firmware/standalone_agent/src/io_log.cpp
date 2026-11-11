#include "io_log.h"

#include <time.h>

#include <vector>

#include "common.h"
#include "storage.h"

namespace vat::iolog {
namespace {

constexpr char kSeq[] = "/vat/io/seq";
constexpr std::size_t kCardLimit = 64 * 1024;   // each stream, then it becomes its .1 (two kept)
constexpr std::size_t kFlashLimit = 16 * 1024;  // no card: the board's flash is 1.5 MB in all
constexpr std::size_t kReplyBytes = 3600;       // under context's inline limit: one read, no out#N
constexpr std::size_t kRunBytes = 2400;         // of one run, when read back
constexpr int kMaxRuns = 8;

struct Stream {
    const char* name;
    const char* label;
};
const Stream kStreams[] = {{"stdin", "标准输入"}, {"stdout", "标准输出"}, {"stderr", "标准错误"}};

String path_of(const char* stream, bool previous = false) {
    return String("/vat/io/") + stream + (previous ? ".1.log" : ".log");
}

String stamp() {
    const time_t now = time(nullptr);
    if (now < 1700000000) return "--:--";  // the clock is not set yet
    struct tm parts;
    localtime_r(&now, &parts);
    char text[20];
    strftime(text, sizeof(text), "%m-%d %H:%M:%S", &parts);
    return text;
}

void append(const char* stream, const String& head, const String& body) {
    const String path = path_of(stream);
    String text = head + body;
    if (!body.isEmpty() && !body.endsWith("\n")) text += "\n";
    const std::size_t limit = storage::kind() == "tf" ? kCardLimit : kFlashLimit;
    if (storage::size(path) + text.length() > limit) storage::rename(path, path_of(stream, true));
    storage::ensure_parent(path);
    storage::append(path, text);
}

struct Entry {
    String head;
    String body;
};

bool is_head(const String& line) { return line.startsWith("== ") && line.endsWith(" =="); }

// "== 11-11 02:00:03 #128 每日备份 来源=触发:每天 结果=失败 耗时=1.2s 内存峰值=18KB =="
bool wanted(const String& head, const String& program, bool failed, long id) {
    const int number = head.indexOf(" #");
    if (number < 0) return false;
    const int name = head.indexOf(' ', number + 2);
    if (name < 0) return false;
    if (id) return head.substring(number + 2, name).toInt() == id;
    if (failed && head.indexOf(" 结果=失败 ") < 0) return false;
    if (program.isEmpty()) return true;
    const int end = head.indexOf(" 来源=", name);
    return end > name && head.substring(name + 1, end) == program;
}

// The runs of one file that are wanted, the newest `count` kept (a line at a
// time: the file may be larger than the largest free block).
void scan(const String& path, std::size_t count, const String& program, bool failed, long id, std::vector<Entry>& found,
          long& total) {
    File file = storage::fs().open(path, FILE_READ);
    if (!file) return;
    bool keeping = false;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (is_head(line)) {
            keeping = wanted(line, program, failed, id);
            if (!keeping) continue;
            ++total;
            found.push_back({line, String()});
            if (found.size() > count) found.erase(found.begin());
        } else if (keeping && found.back().body.length() < kRunBytes) {
            found.back().body += utf8_head(line, kRunBytes - found.back().body.length()) + "\n";
        }
    }
    file.close();
}

}  // namespace

long record(const Run& run) {
    if (!storage::ready()) return 0;
    const long id = storage::read_text(kSeq, 16).toInt() + 1;
    storage::ensure_parent(kSeq);
    storage::overwrite(kSeq, String(id));
    const String head = "== " + stamp() + " #" + String(id) + " " + run.program + " 来源=" + run.origin + " 结果=" +
                        (run.ok ? "成功" : "失败") + " 耗时=" + String(run.ms / 1000.0, 1) + "s 内存峰值=" +
                        String(static_cast<unsigned>((run.peak + 512) / 1024)) + "KB ==\n";
    append("stdout", head, run.out);
    if (!run.in.isEmpty()) append("stdin", head, run.in);
    if (!run.err.isEmpty()) append("stderr", head, run.err);
    return id;
}

String read(const String& stream, int count, const String& program, bool failed) {
    const Stream* which = nullptr;
    for (const auto& s : kStreams) {
        if (stream == s.name) which = &s;
    }
    if (!which) {
        return "ERROR: 用 debug://stdout、debug://stderr、debug://stdin（可带 ?n=最近几次&prog=程序名&failed=1）或 "
               "debug://run/编号";
    }
    std::vector<Entry> found;
    long total = 0;
    const std::size_t keep = static_cast<std::size_t>(constrain(count, 1, kMaxRuns));
    scan(path_of(which->name, true), keep, program, failed, 0, found, total);
    scan(path_of(which->name), keep, program, failed, 0, found, total);
    if (found.empty()) return String(which->label) + "流里没有符合条件的运行";
    String body;
    int shown = 0;
    for (int i = static_cast<int>(found.size()) - 1; i >= 0; --i) {  // from the newest back, while it fits
        const String piece = found[i].head + "\n" + found[i].body;
        if (shown && body.length() + piece.length() > kReplyBytes) break;
        body = utf8_head(piece, kReplyBytes) + body;
        ++shown;
    }
    return String(which->label) + "流 · 最近 " + String(shown) + " 次运行（符合条件的共 " + String(total) +
           " 次，最新的在最后）\n" + body;
}

String read_run(long id) {
    String head;
    String parts[3];
    for (int i = 0; i < 3; ++i) {
        std::vector<Entry> found;
        long total = 0;
        scan(path_of(kStreams[i].name, true), 1, String(), false, id, found, total);
        scan(path_of(kStreams[i].name), 1, String(), false, id, found, total);
        if (found.empty()) continue;
        head = found.back().head;
        parts[i] = utf8_head(found.back().body, 1100);
    }
    if (head.isEmpty()) return "ERROR: 没有运行 #" + String(id) + "（read debug://stdout 看最近的运行）";
    String out = head;
    static const char* const kTitles[] = {"【输入】", "【输出】", "【错误输出】"};
    for (int i = 0; i < 3; ++i) {
        String part = parts[i];
        part.trim();
        out += String("\n") + kTitles[i] + "\n" + (part.isEmpty() ? String("（无）") : part);
    }
    return out;
}

}  // namespace vat::iolog
