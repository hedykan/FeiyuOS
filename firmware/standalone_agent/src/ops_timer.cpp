#include "ops_timer.h"

#include <cJSON.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "common.h"
#include "events.h"
#include "storage.h"
#include "ui_state.h"

namespace vat::timer {
namespace {

constexpr char kFile[] = "/vat/things/timers/timers.json";
constexpr std::size_t kMax = 20;

struct Timer {
    time_t due = 0;          // wall clock; 0 when the clock was not set
    uint32_t due_ms = 0;     // millis() deadline when due == 0
    String text;
};

std::vector<Timer> timers;
std::mutex lock;
uint32_t last_check = 0;

bool clock_set() { return time(nullptr) > 1700000000; }

// A watched time event (event bus); `kind` "" for a one-off at.
struct Watch {
    String target;
    time_t next = 0;
    String kind;
    int hour = 0;
    int minute = 0;
};
std::vector<Watch> watches;  // guarded by `lock`

String format_at(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    char text[24];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
    return text;
}

// Next HH:MM after now on an allowed day (every / weekday / weekend).
time_t next_daily(int hour, int minute, const String& kind) {
    const time_t now = time(nullptr);
    struct tm base;
    localtime_r(&now, &base);
    base.tm_hour = hour;
    base.tm_min = minute;
    base.tm_sec = 0;
    for (int day = 0; day < 8; ++day) {
        struct tm candidate = base;
        candidate.tm_mday += day;
        const time_t when = mktime(&candidate);
        if (when <= now) continue;
        struct tm check;
        localtime_r(&when, &check);
        const bool weekend = check.tm_wday == 0 || check.tm_wday == 6;
        if ((kind == "weekday" && weekend) || (kind == "weekend" && !weekend)) continue;
        return when;
    }
    return 0;
}

void fire_watches() {
    std::vector<String> due;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (watches.empty() || !clock_set()) return;
        const time_t now = time(nullptr);
        auto it = watches.begin();
        while (it != watches.end()) {
            if (it->next > now) {
                ++it;
                continue;
            }
            due.push_back(it->target);
            if (it->kind.isEmpty()) {
                it = watches.erase(it);
            } else {
                it->next = next_daily(it->hour, it->minute, it->kind);
                ++it;
            }
        }
    }
    for (const auto& target : due) events::emit(target);
}

void save() {  // caller holds `lock`
    cJSON* list = cJSON_CreateArray();
    for (const auto& t : timers) {
        if (!t.due) continue;  // relative timers before NTP sync cannot survive a reboot
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "due", static_cast<double>(t.due));
        cJSON_AddStringToObject(item, "text", t.text.c_str());
        cJSON_AddItemToArray(list, item);
    }
    storage::write_text(kFile, json_print(list));
    cJSON_Delete(list);
}

// Seconds in "1h30m", "90s", "10", "2小时", "半小时"; -1 when unreadable.
long parse_duration(String text) {
    text.trim();
    text.toLowerCase();
    text.replace("半小时", "30m");
    text.replace("小时", "h");
    text.replace("分钟", "m");
    text.replace("分", "m");
    text.replace("秒钟", "s");
    text.replace("秒", "s");
    text.replace("天", "d");
    text.replace("min", "m");
    text.replace(" ", "");
    long total = 0, number = 0;
    bool digits = false, any = false;
    for (const char c : text) {
        if (isdigit(static_cast<unsigned char>(c))) {
            number = number * 10 + (c - '0');
            digits = true;
            continue;
        }
        const long unit = c == 's' ? 1 : c == 'm' ? 60 : c == 'h' ? 3600 : c == 'd' ? 86400 : 0;
        if (!unit || !digits) return -1;
        total += number * unit;
        number = 0;
        digits = false;
        any = true;
    }
    if (digits) total += number * 60;  // a bare number means minutes
    return any || digits ? total : -1;
}

// "18:30" (today, or tomorrow when past) / "2026-11-06 08:00" / "...T08:00".
time_t parse_clock(String text) {
    text.trim();
    text.replace("T", " ");
    struct tm now_tm;
    const time_t now = time(nullptr);
    localtime_r(&now, &now_tm);
    struct tm due = now_tm;
    int y, mo, d, h, mi;
    if (sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        due.tm_year = y - 1900;
        due.tm_mon = mo - 1;
        due.tm_mday = d;
    } else if (sscanf(text.c_str(), "%d:%d", &h, &mi) == 2) {
    } else {
        return 0;
    }
    due.tm_hour = h;
    due.tm_min = mi;
    due.tm_sec = 0;
    time_t t = mktime(&due);
    if (text.indexOf('-') < 0 && t <= now) t += 86400;
    return t;
}

String describe(const Timer& t) {
    if (!t.due) {
        const long left = static_cast<long>(t.due_ms - millis()) / 1000;
        return String(left / 60) + " 分 " + String(left % 60) + " 秒后";
    }
    struct tm tm;
    localtime_r(&t.due, &tm);
    char text[24];
    strftime(text, sizeof(text), "%m-%d %H:%M", &tm);
    return text;
}

void sort_timers() {
    std::sort(timers.begin(), timers.end(), [](const Timer& a, const Timer& b) {
        const long long ka = a.due ? static_cast<long long>(a.due) * 1000 : static_cast<long long>(time(nullptr)) * 1000 + (a.due_ms - millis());
        const long long kb = b.due ? static_cast<long long>(b.due) * 1000 : static_cast<long long>(time(nullptr)) * 1000 + (b.due_ms - millis());
        return ka < kb;
    });
}

void fire(const Timer& t) {
    ui::Overlay overlay;
    overlay.kind = ui::Overlay::Kind::Text;
    overlay.text = "提醒\n" + t.text;
    ui::post_overlay(std::move(overlay));
    ui::post_beep(3);
    ui::set_notice("提醒：" + t.text);
    runtime_event("timer", t.text);
}

}  // namespace

void begin() {
    std::lock_guard<std::mutex> guard(lock);
    timers.clear();
    cJSON* list = cJSON_Parse(storage::read_text(kFile, 8192).c_str());
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        Timer t;
        t.due = static_cast<time_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "due")));
        t.text = json_str(item, "text");
        if (t.due && !t.text.isEmpty()) timers.push_back(t);
    }
    cJSON_Delete(list);
}

void loop() {
    if (millis() - last_check < 1000) return;
    last_check = millis();
    fire_watches();
    std::vector<Timer> due;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (timers.empty()) return;
        const time_t now = time(nullptr);
        const bool synced = clock_set();
        auto it = timers.begin();
        while (it != timers.end()) {
            const bool ready = it->due ? (synced && it->due <= now) : static_cast<int32_t>(millis() - it->due_ms) >= 0;
            if (ready) {
                due.push_back(*it);
                it = timers.erase(it);
            } else {
                ++it;
            }
        }
        if (!due.empty()) save();
    }
    for (const auto& t : due) fire(t);  // a reminder missed while off fires at boot
}

String add(const String& kind, const String& when, const String& text) {
    String what = text;
    what.trim();
    if (what.isEmpty()) return "ERROR: 缺少提醒内容";
    Timer t;
    t.text = utf8_head(what, 200);
    if (kind == "in") {
        const long seconds = parse_duration(when);
        if (seconds <= 0 || seconds > 30L * 86400) return "ERROR: 看不懂时长「" + when + "」，例如 timer://in/10m、timer://in/1h30m";
        if (clock_set()) t.due = time(nullptr) + seconds;
        else t.due_ms = millis() + seconds * 1000UL;
    } else if (kind == "at") {
        if (!clock_set()) return "ERROR: 时间还没同步（未联网），只能用 timer://in/时长";
        t.due = parse_clock(when);
        if (!t.due || t.due <= time(nullptr)) return "ERROR: 看不懂或已过去的时间「" + when + "」，例如 timer://at/18:30";
    } else {
        return "ERROR: 用 timer://in/时长 或 timer://at/时刻";
    }
    std::lock_guard<std::mutex> guard(lock);
    if (timers.size() >= kMax) return "ERROR: 提醒已满 20 个，先取消一些";
    timers.push_back(t);
    sort_timers();
    save();
    return "已设提醒：" + describe(t) + "「" + t.text + "」" + (t.due ? "" : "（时间未同步，重启会丢失）");
}

String list_text() {
    std::lock_guard<std::mutex> guard(lock);
    if (timers.empty()) return "没有待触发的提醒";
    String text = String(timers.size()) + " 个提醒：\n";
    int n = 0;
    for (const auto& t : timers) text += String(++n) + ". " + describe(t) + "  " + t.text + "\n";
    return text;
}

String cancel(const String& which) {
    String key = which;
    key.trim();
    std::lock_guard<std::mutex> guard(lock);
    if (timers.empty()) return "没有待触发的提醒";
    if (key == "all" || key == "全部") {
        const int count = timers.size();
        timers.clear();
        save();
        return "已取消全部 " + String(count) + " 个提醒";
    }
    const int index = key.toInt();
    for (std::size_t i = 0; i < timers.size(); ++i) {
        if ((index > 0 && static_cast<std::size_t>(index) == i + 1) || (index <= 0 && !key.isEmpty() && timers[i].text.indexOf(key) >= 0)) {
            const String text = timers[i].text;
            timers.erase(timers.begin() + i);
            save();
            return "已取消提醒「" + text + "」";
        }
    }
    return "ERROR: 没找到要取消的提醒「" + key + "」，先 read timer://list 看编号";
}

String watch(const String& target, String& normalized) {
    if (!clock_set()) return "设备时钟还没校准（联网对时后会自动生效）";
    const int hash = target.indexOf('#');
    const String tag = hash >= 0 ? target.substring(hash) : String();
    const String body = hash >= 0 ? target.substring(0, hash) : target;
    const String rest = body.substring(strlen("time://"));
    const int slash = rest.indexOf('/');
    const String usage = "时间写法：time://at/18:30、time://at/2026-11-08 09:00、time://in/10m、time://every/07:00、"
                         "time://weekday/09:00、time://weekend/10:00";
    if (slash < 0) return usage;
    const String kind = rest.substring(0, slash);
    String value = url_decode(rest.substring(slash + 1));
    value.trim();
    Watch w;
    if (kind == "in") {
        const long seconds = parse_duration(value);
        if (seconds <= 0) return "看不懂时长「" + value + "」，如 10m、1h30m、90s";
        w.next = time(nullptr) + seconds;
    } else if (kind == "at") {
        int y, mo, d, h, mi, sec;
        time_t t = 0;
        if (sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
            struct tm tm = {};
            tm.tm_year = y - 1900;
            tm.tm_mon = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_sec = sec;
            tm.tm_isdst = -1;
            t = mktime(&tm);
        } else {
            t = parse_clock(value);
        }
        if (!t) return "看不懂时刻「" + value + "」。" + usage;
        if (t <= time(nullptr)) return "时刻「" + value + "」已经过去";
        w.next = t;
    } else if (kind == "every" || kind == "weekday" || kind == "weekend") {
        int h = -1, mi = -1;
        if (sscanf(value.c_str(), "%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) {
            return "每天的时刻要写成 HH:MM，如 time://every/07:00";
        }
        w.kind = kind;
        w.hour = h;
        w.minute = mi;
        w.next = next_daily(h, mi, kind);
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", h, mi);
        normalized = "time://" + kind + "/" + hm + tag;
    } else {
        return usage;
    }
    if (w.kind.isEmpty()) normalized = "time://at/" + format_at(w.next) + tag;
    w.target = normalized;
    std::lock_guard<std::mutex> guard(lock);
    watches.erase(std::remove_if(watches.begin(), watches.end(), [&](const Watch& x) { return x.target == w.target; }),
                  watches.end());
    watches.push_back(w);
    return "";
}

void unwatch(const String& target) {
    std::lock_guard<std::mutex> guard(lock);
    watches.erase(std::remove_if(watches.begin(), watches.end(), [&](const Watch& x) { return x.target == target; }),
                  watches.end());
}

long duration_seconds(const String& text) { return parse_duration(text); }

}  // namespace vat::timer
