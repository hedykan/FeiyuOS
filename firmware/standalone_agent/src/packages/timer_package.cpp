// timer: reminders on the device itself (see ops_timer).
//   write timer://in/10m        content: what to remind   (also 1h30m, 90s, 2小时)
//   write timer://at/18:30      content: what to remind   (or 2026-11-06 08:00)
//   read  timer://list
//   write timer://N/delete      N: number from the list, a word of the text, or all
#include "../common.h"
#include "../ops_timer.h"
#include "../registry.h"

namespace vat::registry {
namespace {


// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "定时提醒用 write target=timer://in/时长（如 10m、1h30m）或 timer://at/时刻（如 18:30），content 只写提醒内容，查看用 read timer://list，取消用 write timer://编号/delete（全部 timer://all/delete）。到点要执行动作（开空调、发消息、跑命令）不要用 timer，用 trigger://（见程序包）。";

class TimerPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "timer 到点只会弹提醒、响铃，不会做任何事：write timer://in/时长（10m、1h30m）或 timer://at/时刻（18:30），read timer://list；到点要执行动作（开关电器、发消息、跑命令）就写成程序用 trigger://new 定时"; }
    bool reversible(const String&, const cJSON*) const override { return true; }

    const Manifest& manifest() const override {
        static const Manifest m{"timer", "本机定时提醒：多久以后或几点提醒，到点全屏显示并响铃",
                                "提醒 定时 闹钟 计时 倒计时 分钟后 小时后 几点 叫我 别忘了 待会 取消提醒 有哪些提醒", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("timer://");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String rest = url_decode(json_str(arguments, "target").substring(strlen("timer://")));
        if (tool == "read" || rest == "list" || rest.isEmpty()) return timer::list_text();
        const String content = json_str(arguments, "content");
        if (rest.endsWith("/delete")) return timer::cancel(rest.substring(0, rest.length() - 7));
        const int slash = rest.indexOf('/');
        if (slash < 0) return "ERROR: 用 timer://in/10m 或 timer://at/18:30，content 写提醒内容";
        return timer::add(rest.substring(0, slash), rest.substring(slash + 1), content);
    }

    void hints(std::vector<Hint>& out) override { out.push_back({"提醒", "", "十分钟后提醒我喝水"}); }

    void triggers(std::vector<TriggerSpec>& out) const override {
        out.push_back({"time://at/18:30", "某个时刻一次（也可 2026-11-08 09:00）"});
        out.push_back({"time://in/10m", "多久以后一次"});
        out.push_back({"time://every/07:00", "每天这个时刻"});
        out.push_back({"time://every/30s", "每隔多久一次（30s、5m、2h；至少 10 秒；忙时跳过这一轮）"});
        out.push_back({"time://weekday/09:00", "工作日这个时刻"});
        out.push_back({"time://weekend/10:00", "周末这个时刻"});
    }

    bool watch(const String& target, String& normalized, String& error) override {
        if (!target.startsWith("time://")) return false;
        error = timer::watch(target, normalized);
        return true;
    }

    void unwatch(const String& target) override {
        if (target.startsWith("time://")) timer::unwatch(target);
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"timer://in/10m", "timer：多久以后提醒（10m、1h30m、90s），write content 为提醒内容"},
            {"timer://at/18:30", "timer：几点提醒（18:30 或 2026-11-06 08:00），write content 为提醒内容"},
            {"timer://list", "timer：read 列出待触发的提醒"},
            {"timer://编号/delete", "timer：write 取消提醒，编号也可以是内容里的词或 all"},
        };
        for (const auto& item : items) {
            add_resource(list, remaining, item[0], item[1]);
        }
    }
};

}  // namespace

Package* make_timer_package() { return new TimerPackage(); }

}  // namespace vat::registry
