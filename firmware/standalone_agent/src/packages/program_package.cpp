// program: programs (prog://) — JSON step lists the agent writes and the
// device runs — and triggers (trigger://) that run a step on a time or an
// event.  The language is in prog_engine, storage and runs in prog_runtime;
// design in docs/PROGRAMS.zh-CN.md.
//   write prog://名字            content: program JSON (create or replace)
//   exec  prog://名字?参数=值    run it
//   read  prog://list | prog://名字 | prog://help | prog://unused | prog://runs
//   write prog://名字/delete | prog://cleanup
//   read  trigger://list
//   write trigger://new          content: {"when": "...", "run": {one step}}
//   write trigger://编号/pause | /resume | /delete
#include "../common.h"
#include "../events.h"
#include "../prog_engine.h"
#include "../prog_runtime.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt: when to use it and where the details are.
// JSON-safe text only (no double quotes or backslashes).
constexpr char kUsage[] =
    "程序与触发（prog:// 与 trigger://）：用户要把几件事组合起来、反复做、定时做或开机做时使用。程序是一串步骤，存为 "
    "prog://名字，随时 exec 运行；触发把时间或事件绑到一个步骤上（通常是运行某个程序）。写或改程序前先 read prog://list "
    "看已有的，能复用就调用；拿不准写法时 read prog://help。步骤的写法：一个键是动作（read、write、exec 等），值是 target，"
    "其余字段是该动作的参数，例如键 write 值 device://local/led、content 为 off。保存程序 write target=prog://名字，content "
    "为 JSON，含 name 与 steps（步骤列表）；运行 exec target=prog://名字?参数=值（program 填 run，args 为空）。新建触发 "
    "write target=trigger://new，content 为 JSON，含 when 与 run：when 是事件字符串，如 time://every/07:00（每天）、"
    "time://weekday/09:00、time://at/18:30（一次）、time://in/10m（一次）、device://boot（开机），不要用 cron；run 是一个步骤，"
    "写法同上，如键 exec 值 prog://早安。只做一件事时直接建触发，run 里写那个动作；多件事先写程序再建触发运行它。查看 "
    "read trigger://list；停用 write trigger://编号/pause；取消正在等待的一次运行（程序保留）write prog://runs/编号/cancel，编号见 read prog://runs；删除 write prog://名字/delete 或 trigger://编号/delete；清理先 "
    "read prog://unused 再 write prog://cleanup。";

// read prog://help: the whole language with examples (read on demand, so it
// costs nothing in the system prompt).
constexpr char kHelp[] = R"HELP(程序（prog://）与触发（trigger://）说明

【程序】{"name": 名字, "about": 一句说明（可选）, "input": 参数（可选）, "steps": [步骤…]}
步骤只有这几种，每步一个键：
- 动作：read / write / exec / observe / input / session，值是 target，其余字段就是该动作平时的参数（content、program、args…）。mode、confirm_required 不用写。
- {"if": "变量名", "is" | "not" | "has" | "above" | "below": 值, "then": [步骤…], "else": [步骤…]}  变量名不加花括号；has 是包含；above/below 按数字比较。
- {"repeat": 次数(1-10), "steps": [步骤…]}
- {"wait": "25m"}  单位 s/m/h，最长 24h；等待时不占设备，到点从原处继续。
任何动作可加 "as": "名字"（把结果存成变量）和 "on_error": "continue"（失败也继续；默认失败就停）。
变量：{名字} 是 as 存的结果，{input.名字} 是参数，{event.字段} 是触发事件的数据，{error} 是上一个失败的原因，{now} 是当前时间。
问大肥鱼：{"write": "ask://", "content": "问题，只答 是/否", "as": "x"}，再用 if 判断 x。能用确定的步骤完成的不要用 ask://（每次都要联网、调用一次模型）；ask:// 里不能执行需要确认的操作。
调用另一个程序：{"exec": "prog://名字?参数=值", "as": "r"}，被调用程序最后一步的结果就是返回值。
参数："input": {"to": null, "mode": "walk"}：null 是必填，写了值就是默认值。
上限：每个程序 40 步、嵌套 4 层；一次运行最多执行 200 步；调用最多 3 层，不能互相调用成环。
需要确认的步骤（远端命令、写文件、红外原始码…）在保存程序时由用户确认一次，之后运行不再确认；需要密码的操作不能放进程序。

【触发】{"when": 事件, "run": 一个步骤}
时间：time://at/18:30（或 2026-11-08 09:00）一次、time://in/10m 一次、time://every/07:00 每天、time://weekday/09:00 工作日、time://weekend/10:00 周末；开机：device://boot。一次性的触发执行后自动删除。可用事件见 discover(resources) 里标“可作触发”的项。

【例子】
1) 10 分钟后提醒关火（一件事，直接建触发）：
write trigger://new  content={"when":"time://in/10m","run":{"write":"device://local/display/banner","content":"该关火了！"}}

2) 早安：每天 7 点响铃、查天气，下雨就亮蓝灯（程序 + 触发）：
write prog://早安  content={"name":"早安","steps":[
 {"write":"device://local/audio/beep","content":"3"},
 {"read":"place://weather","as":"weather"},
 {"write":"ask://","content":"天气：{weather}。今天会下雨吗？只答 是 或 否","as":"rain"},
 {"if":"rain","is":"是","then":[{"write":"device://local/led","content":"蓝"}],"else":[{"write":"device://local/led","content":"绿"}]},
 {"write":"device://local/display/text","content":"早安！{weather}"}]}
write trigger://new  content={"when":"time://every/07:00","run":{"exec":"prog://早安"}}

3) 函数与参数：
write prog://走去  content={"name":"走去","input":{"to":null,"mode":"walk"},"steps":[
 {"read":"place://route?to={input.to}&mode={input.mode}","as":"r"},
 {"write":"device://local/display/text","content":"{r}"}]}
exec prog://走去?to=深圳北站&mode=bike

更多例子（番茄钟 repeat + wait、睡前关电视、我出门了、管理命令）：read prog://help/examples
)HELP";

constexpr char kHelpExamples[] = R"HELP(程序与触发：更多例子

番茄钟（repeat + wait，手动运行）：
write prog://番茄钟  content={"name":"番茄钟","steps":[{"repeat":4,"steps":[
 {"write":"device://local/led","content":"红"},{"write":"device://local/audio/beep","content":"1"},{"wait":"25m"},
 {"write":"device://local/led","content":"绿"},{"write":"device://local/audio/beep","content":"2"},{"wait":"5m"}]},
 {"write":"device://local/display/banner","content":"番茄钟完成"}]}
exec prog://番茄钟

睡前：每天 23:00 关电视、关灯：
write prog://睡前  content={"name":"睡前","steps":[{"write":"ir://电视/power","content":"","on_error":"continue"},{"write":"device://local/led","content":"off"}]}
write trigger://new  content={"when":"time://every/23:00","run":{"exec":"prog://睡前"}}

我出门了（调用另一个程序，失败也继续）：
write prog://我出门了  content={"name":"我出门了","steps":[
 {"exec":"prog://睡前","on_error":"continue"},
 {"exec":"prog://走去?to=公司"}]}

管理：read prog://list；read prog://早安（大纲）；read trigger://list；write trigger://3/pause；write trigger://3/resume；
write prog://番茄钟/delete；read prog://unused；write prog://cleanup；read prog://runs（等待中与最近的运行）；write prog://runs/2/cancel（取消等待中的那次，程序保留）。
)HELP";

const char* const kReserved[] = {"list", "help", "unused", "runs", "cleanup"};

bool reserved(const String& name) {
    for (const char* r : kReserved) {
        if (name == r) return true;
    }
    return false;
}

// "prog://名字/delete?x" -> path "名字/delete" (decoded), query "x".
void split(const String& target, std::size_t scheme, String& path, String& query) {
    const String rest = target.substring(scheme);
    const int mark = rest.indexOf('?');
    path = url_decode(mark < 0 ? rest : rest.substring(0, mark));
    query = mark < 0 ? String() : rest.substring(mark + 1);
    while (path.endsWith("/")) path.remove(path.length() - 1);
}

class ProgramPackage : public Package {
public:
    ProgramPackage() { events::subscribe(programs::on_event); }

    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"program", "程序与触发：把动作组合成可反复运行的程序（prog://），按时间或开机自动运行（trigger://）",
                                "程序 函数 脚本 组合 宏 一键 自动 自动化 定时 每天 每周 工作日 周末 开机 触发 闹钟 番茄钟 场景 "
                                "到点 以后每天 帮我设置 program prog trigger",
                                {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        if (tool != "read" && tool != "write" && tool != "exec") return false;
        const String target = json_str(arguments, "target");
        return target.startsWith("prog://") || target.startsWith("trigger://");
    }

    bool reversible(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return true;  // read; exec runs steps confirmed when the program was saved
        const String target = json_str(arguments, "target");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (path == "new") return !programs::trigger_needs_confirm(json_str(arguments, "content"));
            return !path.endsWith("/delete");  // pause / resume
        }
        split(target, strlen("prog://"), path, query);
        if (path.startsWith("runs/")) return true;  // cancel a waiting run: what was asked, nothing is lost
        if (path == "cleanup" || path.endsWith("/delete")) return false;
        if (path.endsWith("/run")) return true;
        return !programs::needs_confirm(json_str(arguments, "content"));
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return {};
        const String target = json_str(arguments, "target");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (path == "new") return programs::trigger_summary(json_str(arguments, "content"));
            if (path.endsWith("/delete")) return "删除触发 " + programs::trigger_line(path.toInt());
            return {};
        }
        split(target, strlen("prog://"), path, query);
        if (path == "cleanup") return "删除这些不用的程序：" + programs::cleanup_list();
        if (path.endsWith("/delete")) return "删除程序「" + path.substring(0, path.length() - 7) + "」";
        return programs::save_summary(path, json_str(arguments, "content"));
    }

    String execute(const String& tool, const cJSON* arguments, bool approved) override {
        const String target = json_str(arguments, "target");
        const String content = json_str(arguments, "content");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (tool == "read" || path.isEmpty() || path == "list") return programs::triggers_text();
            if (path == "new") return programs::add_trigger(content, approved);
            const int slash = path.indexOf('/');
            if (slash < 0) return "ERROR: 用 trigger://编号/pause、/resume 或 /delete";
            return programs::trigger_action(path.substring(0, slash).toInt(), path.substring(slash + 1), approved);
        }
        split(target, strlen("prog://"), path, query);
        if (tool == "exec" || path.endsWith("/run")) {
            if (path.endsWith("/run")) path = path.substring(0, path.length() - 4);
            return programs::run(path, query);
        }
        if (tool == "read") {
            if (path.isEmpty() || path == "list") return programs::list_text();
            if (path == "help") return kHelp;
            if (path == "help/examples") return kHelpExamples;
            if (path == "unused") return programs::unused_text();
            if (path == "runs") return programs::runs_text();
            return programs::show(path);
        }
        if (path == "cleanup") return programs::cleanup();
        if (path.startsWith("runs/")) {
            if (!path.endsWith("/cancel")) return "ERROR: 取消等待中的运行用 write prog://runs/编号/cancel";
            return programs::cancel_run(path.substring(5, path.length() - 7).toInt());
        }
        if (path.endsWith("/delete")) return programs::remove(path.substring(0, path.length() - 7));
        if (reserved(path)) return "ERROR: 程序名不能是 list、help、unused、runs、cleanup";
        if (path.indexOf('/') >= 0) return "ERROR: 保存用 write prog://名字，删除用 prog://名字/delete";
        return programs::save(path, content, approved);
    }

    void loop() override { programs::loop(); }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"程序与定时", "", "每天早上 7 点叫我起床并说天气"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"prog://list", "program：read 列出程序（签名、调用关系、触发、上次运行）"},
            {"prog://help", "program：read 程序与触发的完整写法和例子"},
            {"prog://名字", "program：write 保存程序（content 为程序 JSON）；exec 运行；read 看大纲"},
            {"trigger://new", "program：write 新建触发，content 为 {when, run}"},
            {"trigger://list", "program：read 列出触发"},
            {"prog://runs", "program：read 等待中与最近的运行"},
            {"prog://runs/编号/cancel", "program：write 取消一次等待中的运行（程序和触发保留）"},
        };
        for (const auto& item : items) {
            if (!add_resource(list, remaining, item[0], item[1])) return;
        }
    }
};

}  // namespace

Package* make_program_package() { return new ProgramPackage(); }

}  // namespace vat::registry
