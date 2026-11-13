// program: Lua programs (prog://) the agent writes and the device runs, and
// triggers (trigger://) that run a program on a time or an event.  Files and
// runs in prog_runtime, the VM in lua_vm; design in docs/PROGRAMS.zh-CN.md.
//   write prog://名字            content: Lua source (create or replace)
//   write prog://名字/patch      content: <<<< old ==== new >>>> blocks (replace parts)
//   read  prog://名字?grep=词 | ?lines=开始-结束   part of the source
//   exec  prog://名字?参数=值    run it
//   read  prog://list | prog://名字 | prog://help | prog://help/examples | prog://legacy/名字
//   write prog://名字/delete
//   read  trigger://list
//   write trigger://new          content: {"when": "...", "run": "prog://名字?参数"}
//   write trigger://编号/pause | /resume | /delete
//   write trigger://clean        (confirmed) triggers of deleted programs / paused after failing
#include <new>
#include "../common.h"
#include "../events.h"
#include "../prog_runtime.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt: when to use it and where the details are.
// JSON-safe text only (no double quotes or backslashes).
constexpr char kUsage[] =
    "程序与触发（prog:// 与 trigger://）：用户要把几件事组合起来、反复做、定时做或开机做时使用。程序用 Lua 写，存为 "
    "prog://名字：write target=prog://名字，content 为 Lua 源码；运行 exec target=prog://名字?参数=值（program 填 run，args "
    "为空）。写或改程序前先 read prog://help 看写法（vat.read、vat.write、vat.exec 调用各项能力，需要确认的能力在开头用 "
    "-- @uses 声明，保存时由用户确认一次），并 read prog://list 看已有的，能复用就调用。用户问有哪些程序、哪些可以清理或删掉时，"
    "先 read prog://list（每个程序的说明、最近运行、被哪些触发使用），据此建议，删除仍要用户确认。新建触发 write "
    "target=trigger://new，content 为 JSON，含 when 与 run：when 是事件，如 time://every/07:00（每天）、time://every/30s（每隔多久，至少 10 秒，设备忙时跳过这一轮）、"
    "time://weekday/09:00、time://at/18:30（一次）、time://in/10m（一次）、device://local/event/boot（开机），不要用 cron；run 是 "
    "prog://程序名?参数。只做一件事也写成一两行的程序再建触发。查看 read trigger://list；停用 write trigger://编号/pause；清理失效的触发 write trigger://clean；"
    "删除 write prog://名字/delete 或 trigger://编号/delete（用户要删时直接调用，设备会弹出确认，不要先用文字反问）。"
    "写程序：保存后必须 exec 运行一次，看结果和输出；有错就改，改完再运行，直到成功。测试覆盖主要路径即可，一般用 1 到 2 组输入跑一遍，"
    "都通过就交付，出错时才继续改和测。程序的输入有两种，不要混用：运行前就定好的参数用 -- @input 声明，运行时写在地址里（prog://名字?n=10）；"
    "运行中要用户在键盘上输入的（用户说让我输入、问我、等我回答时就是这种）用 io.read 或 vat.readline 读，"
    "测试时把输入放在 exec 的 args 里，每一项就是一行输入，多次输入就放多项；不给的话程序会一直等用户在设备上输入。"
    "程序里能力调用的返回值：有数据的调用返回一张表，用一个变量接住再取字段，不要解析文字："
    "local b = vat.read('device://local/battery')，取 b.percent、b.voltage、b.usb；"
    "local a = vat.read('sensor://imu/acceleration')（gyro 同），取 a.x、a.y、a.z，orientation 取 pitch、roll；"
    "读写 device://local/led 的结果取 color；"
    "local r = vat.exec('ssh://主机', '命令', {参数})，取 r.stdout、r.stderr、r.code（code 不是 0 就是命令失败）；"
    "local r, err = vat.exec('prog://名字', 'run', {输入})，成功时取 r.result（对方 return 的值）、r.stdout、r.stderr，要显示或比较时用 r.result 或 r.stdout。"
    "其他调用返回文字。任何调用失败（包括被调用的程序出错）都返回 nil 和原因，写成 local v, err = vat.read(...)，先判断 v 是不是 nil。"
    "vat.log 写到标准错误并带时间，不占标准输出。"
    "改已有的程序：大程序不用整份读，read prog://名字?grep=关键词 看匹配的行（带行号），read prog://名字?lines=120-160 看其中一段原文。"
    "只改一小部分时不用整份重写：write prog://名字/patch，content 的每一段依次是：一行 <<<<，要替换的原文（和程序里完全一致，包括缩进，"
    "并且只出现一次），一行 ====，替换成的新文字，一行 >>>>；可以连着写多段。新写程序或大改仍然用 write prog://名字 整份保存。";

// read prog://help: the whole language (read on demand, costs nothing in the prompt).
constexpr char kHelp[] = R"HELP(程序（prog://，Lua 5.4）与触发（trigger://）说明

【保存与运行】write prog://名字，content 是 Lua 源码；exec prog://名字?参数=值 运行。程序的结果是它 return 的值（转成文字）；print 的内容记进运行输出，用户让你运行时还会实时显示在设备的程序页上。

【写程序的做法】保存后一定 exec 运行一次，看结果和输出；有错就改，改完再运行，直到成功，用 1 到 2 组输入覆盖主要路径即可。程序的输入有两种，不要混用：运行前定好的参数用 -- @input 声明，运行时写在地址里（prog://名字?n=10）；运行中要用户在键盘上输入的（用户说让我输入、问我、等我回答）用 io.read 或 vat.readline 读，测试时把输入放进 exec 的 args，每一项是一行，不给的话程序会一直等用户在设备上输入。有数据的调用用变量接住再取字段（见【返回值】），不解析文字；任何调用都可能返回 nil 和原因，先判断再用。只做一件事也写成一两行的程序再建触发。用户问哪些程序可以删时先 read prog://list（说明、最近运行、被哪些触发使用）再建议；删除直接调用，设备会弹确认。

【开头声明】文件开头的注释行，代码之前：
-- @about 一句话说明（列表里显示）
-- @input city 深圳        参数和默认值；不写默认值就是必填。程序里用 input.city（都是字符串，数字用 tonumber）
-- @uses exec ssh://开发机  需要确认的能力：动作 + 目标，目标可以用 * 结尾表示前缀，如 write device://local/display/*
会改变外部东西的能力（远端命令、写远端文件、红外遥控家电、发消息、联网写入……）必须在 @uses 里声明，保存时用户确认一次，之后运行不再确认；没声明的会在运行时被拒绝。不用声明：读取类（读电量、读天气、只读命令），以及本机可撤销的操作（屏幕显示、LED、声音、音量、剪贴板、定时提醒 timer://、面板 ui://）——多声明会让用户多确认一次。需要输密码的操作不能放进程序。

【vat：调用设备的各项能力】（和你平时的工具一样，失败时返回 nil 和错误原因）
vat.read(target[, 选项])                     例 vat.read("device://local/battery")
vat.write(target, 内容[, 选项])              内容是表时自动转成 JSON
vat.exec(target, 命令[, {参数…}[, 选项]])    例 vat.exec("ssh://开发机", "df", {"-h", "/"})
vat.observe(target)  vat.input(target, 文字[, {keys=…, submit=true}])  vat.session(动作[, 选项])
vat.stream("sensor://imu/acceleration")      取流的最新值，返回表，如 a.x a.y a.z
vat.after("25m", "prog://名字?参数")          过一段时间再运行某个程序（一次；重启后仍有效）
vat.sleep(毫秒)                               原地等待，最长 60000（一分钟）；更久用 vat.after
vat.json(文字) 解析 JSON 成表；vat.tojson(表) 转成 JSON 文字；vat.urlencode(文字)
vat.millis() 开机以来的毫秒；vat.now() 得到 {ms=…, time="14:03:59", date="2026-11-11"}（没对时的时候只有 ms）
print(...) 写标准输出（io.write 不换行；print 一个表会显示成 JSON）；vat.log(...) 写标准错误并带上程序开始以来的时间，不占输出的 2 KB（io.stderr:write 也写标准错误）
io.read() / vat.readline("提示")              读用户在键盘上输入的一行（io.read("n") 读数字）；exec 时给 args（每项一行）或 stdin="第一行\n第二行" 就从它读，读完返回 nil
vat.key(超时毫秒)                              读一个按键（"a" "enter" "del"；; . , / 当上下左右），超时返回 nil
vat.clear() 清空程序页；vat.interactive() 有人在看（用户让你运行或在程序页启动；触发器在后台运行时为 false，读输入返回 nil）
调用另一个程序：local r = vat.exec("prog://名字?x=1", "run", {"第一行输入"})，成功时 r 是表：r.result（对方 return 的结果）、r.stdout、r.stderr；对方出错时和别的调用一样返回 nil 和原因（最多嵌套 3 层）
【返回值】有数据的调用返回表，直接取字段：电量 {percent, voltage, usb}；sensor://imu/acceleration、gyro {x, y, z}，orientation {pitch, roll}；灯 {color}；ssh 的 exec {stdout, stderr, code}；程序的 exec 见上。
这些表当文字用时就是原来那句话：tostring(r)、r .. ""、r:match(...) 都作用在文字上；要原文也可以加选项 {text=true}。其他调用返回文字。调用失败返回 nil 和原因，并在标准错误里记一行带行号的警告。
【改程序】只改一部分：write prog://名字/patch，content 写成若干段
<<<<
要替换的原文（和程序里完全一致，并且只出现一次）
====
新文字
>>>>
看程序的一部分：read prog://名字?grep=关键词（带行号）或 read prog://名字?lines=120-160。
【运行记录】每次运行的输入、输出、错误记在三条全局的流里：read debug://stdout、debug://stderr、debug://stdin（?n=最近几次&prog=名字&failed=1），read debug://run/编号 看一次运行的全部。

【规则与上限】单次运行最长 150 秒（sleep 也算）；内存 48 KB，所以一个程序的源码大约 7 KB 是上限（每次调用另一个程序都是重新开始，不共用数据）；死循环会被停下。后台运行（触发）的程序不要长时间 sleep，分段用 vat.after。可用的 Lua 库：string table math utf8 coroutine；没有 io、os、文件和网络库，一切通过 vat。

【触发】新建：write trigger://new，content 为 {"when": 事件, "run": "prog://名字?参数"}
时间：time://at/18:30（或 2026-11-08 09:00）一次、time://in/10m 一次、time://every/07:00 每天、time://weekday/09:00 工作日、time://weekend/10:00 周末；开机：device://local/event/boot；后台运行的程序失败：prog://event/failed（input.event.program、failures、error）。一次性的触发执行后自动删除。可用事件见 discover(resources) 里标「可作触发」的项。事件的数据在程序里是 input.event。

【管理】read prog://list；read prog://名字（源码）；write prog://名字/delete；read trigger://list；write trigger://3/pause | /resume | /delete。卡上全部程序和扩展包：read pkg://list。
旧的 JSON 程序已不能运行：read prog://legacy/名字 看原文，翻译成 Lua 后 write prog://名字 保存。

更多例子：read prog://help/examples
)HELP";

constexpr char kHelpExamples[] = R"HELP(程序例子

早安（触发：每天 07:00）：
-- @about 早上播报天气，下雨提醒带伞
-- @uses write device://local/display/*
local w = vat.read("place://weather") or "天气未知"
local text = "早上好，" .. w
if w:find("雨") then text = text .. "，记得带伞" end
vat.write("device://local/display/text", text)
return text

开发机磁盘检查（只读命令，不用声明）：
-- @about 看开发机根分区用了多少
-- @input host 开发机
local out, err = vat.exec("ssh://" .. input.host, "df", {"-h", "/"})
if not out then return "失败：" .. err end
return out:match("(%d+%%)") or out

番茄钟（分段，不长时间占着设备）：
-- @about 25 分钟专注 + 5 分钟休息，共 4 轮
-- @input round 1
-- @uses write device://local/display/*
local r = tonumber(input.round)
vat.write("device://local/display/text", "第 " .. r .. " 轮：专注 25 分钟")
vat.write("timer://in/25m", "第 " .. r .. " 轮结束，休息 5 分钟")
if r < 4 then vat.after("30m", "prog://番茄钟?round=" .. (r + 1)) end
return "第 " .. r .. " 轮开始"

面板（显示几个选项，用户选了再处理：触发监听 ui://panel/灯光，run 填 prog://灯光处理）：
-- @about 灯光面板
vat.write("ui://panel/灯光", {title = "灯光", choices = {"开", "关"}})
-- 另一个程序 灯光处理：
-- @uses write device://local/led
if input.event.choice == "开" then vat.write("device://local/led", "白") else vat.write("device://local/led", "off") end

摇一摇计数（读流）：
local n = 0
for i = 1, 50 do
  local a = vat.stream("sensor://imu/acceleration")
  if a and math.abs(a.x) + math.abs(a.y) > 1.5 then n = n + 1 end
  vat.sleep(100)
end
return "晃了 " .. n .. " 次"

建触发：write trigger://new  content={"when":"time://every/07:00","run":"prog://早安"}
)HELP";

const char* const kReserved[] = {"list", "help", "legacy"};

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
    const char* index() const override { return "写或改程序前先 read prog://help；prog://list、运行用 exec target=prog://名字、program=run、定时或开机运行 write trigger://new（content 为 {when, run}），read trigger://list，write trigger://编号/delete 或 /pause，清理失效的 write trigger://clean"; }

    const Manifest& manifest() const override {
        static const Manifest m{"program", "程序与触发：用 Lua 把动作组合成可反复运行的程序（prog://），按时间或开机自动运行（trigger://）",
                                "程序 函数 脚本 lua 组合 宏 一键 自动 自动化 定时 每天 每周 工作日 周末 开机 触发 闹钟 番茄钟 场景 "
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
        if (tool != "write") return true;  // read; exec runs what was confirmed when the program was saved
        const String target = json_str(arguments, "target");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (path == "clean") return false;
            return !path.endsWith("/delete");  // new / pause / resume: a trigger only runs confirmed programs
        }
        split(target, strlen("prog://"), path, query);
        if (path.endsWith("/delete")) return false;
        if (path.endsWith("/patch")) {
            return !programs::patch_needs_confirm(path.substring(0, path.length() - 6), json_str(arguments, "content"));
        }
        return !programs::needs_confirm(json_str(arguments, "content"));
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return {};
        const String target = json_str(arguments, "target");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (path.endsWith("/delete")) return "删除触发 " + programs::trigger_line(path.toInt());
            if (path == "clean") return programs::trigger_clean_summary();
            return {};
        }
        split(target, strlen("prog://"), path, query);
        if (path.endsWith("/delete")) return "删除程序「" + path.substring(0, path.length() - 7) + "」";
        if (path.endsWith("/patch")) {
            return programs::patch_summary(path.substring(0, path.length() - 6), json_str(arguments, "content"));
        }
        return programs::save_summary(path, json_str(arguments, "content"));
    }

    String execute(const String& tool, const cJSON* arguments, bool approved) override {
        const String target = json_str(arguments, "target");
        const String content = json_str(arguments, "content");
        String path, query;
        if (target.startsWith("trigger://")) {
            split(target, strlen("trigger://"), path, query);
            if (tool == "read" || path.isEmpty() || path == "list") return programs::triggers_text();
            if (path == "new") return programs::add_trigger(content);
            if (path == "clean") return programs::trigger_clean(approved);
            const int slash = path.indexOf('/');
            if (slash < 0) return "ERROR: 用 trigger://编号/pause、/resume 或 /delete";
            return programs::trigger_action(path.substring(0, slash).toInt(), path.substring(slash + 1), approved);
        }
        split(target, strlen("prog://"), path, query);
        if (tool == "exec") {
            // What the program reads: the stdin text, or the args, one line each.
            String input = json_str(arguments, "stdin");
            if (input.isEmpty()) {
                const cJSON* item = nullptr;
                cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(arguments, "args")) {
                    if (!input.isEmpty()) input += "\n";
                    if (cJSON_IsString(item)) input += item->valuestring;
                    else if (cJSON_IsNumber(item)) input += String(item->valuedouble, item->valuedouble == static_cast<long>(item->valuedouble) ? 0 : 4);
                }
            }
            return programs::run(path, query, input);
        }
        if (tool == "read") {
            if (path.isEmpty() || path == "list") return programs::list_text();
            if (path == "help") return kHelp;
            if (path == "help/examples") return kHelpExamples;
            if (path == "legacy") return programs::legacy_text();
            if (path.startsWith("legacy/")) return programs::legacy_show(path.substring(7));
            if (!query.isEmpty()) return programs::show_part(path, query);
            return programs::show(path);
        }
        if (path.endsWith("/delete")) {
            if (!approved) return "ERROR: 删除程序需要用户确认";
            return programs::remove(path.substring(0, path.length() - 7));
        }
        if (path.endsWith("/patch")) return programs::patch(path.substring(0, path.length() - 6), content, approved);
        if (reserved(path)) return "ERROR: 程序名不能是 list、help、legacy";
        if (path.indexOf('/') >= 0) return "ERROR: 保存用 write prog://名字，删除用 prog://名字/delete";
        return programs::save(path, content, approved);
    }

    void loop() override { programs::loop(); }

    void triggers(std::vector<TriggerSpec>& out) const override {
        out.push_back({"prog://event/failed",
                       "后台运行（触发、vat.after、开机）的程序失败时；input.event 里有 program、failures（连续失败次数）、run（运行编号）、error"});
    }

    bool watch(const String& target, String& normalized, String&) override {
        if (target != "prog://event/failed") return false;
        normalized = target;  // emitted by every failed background run
        return true;
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"程序与定时", "", "每天早上 7 点叫我起床并说天气"});
    }

    // A saved program the query names: without this, "修一下工具箱" sends the model
    // looking for a project called 工具箱 on some host.
    void find(const std::vector<String>& terms, cJSON* list, int& remaining) override {
        std::vector<programs::ProgramInfo> saved;
        try {
            saved = programs::runnable();
        } catch (const std::bad_alloc&) {
            runtime_event("low_memory", "discover programs");
        }
        for (const auto& p : saved) {
            String hay = p.name + " " + p.about;
            hay.toLowerCase();
            bool hit = false;
            String name = p.name;
            name.toLowerCase();
            for (const auto& term : terms) hit = hit || term == name || (term.length() > 3 && hay.indexOf(term) >= 0);
            if (!hit) continue;
            String kind = "program：已保存的程序";
            if (!p.about.isEmpty()) kind += "：" + utf8_head(p.about, 90);
            if (!add_resource(list, remaining, ("prog://" + p.name).c_str(), kind.c_str())) return;
        }
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"prog://list", "program：read 列出程序（参数、状态、被哪些触发使用、最近运行）"},
            {"prog://help", "program：read 用 Lua 写程序的完整说明（vat 接口、@uses 声明、上限）"},
            {"prog://help/examples", "program：read 程序例子"},
            {"prog://名字", "program：write 保存程序（content 为 Lua 源码）；exec 运行；read 看源码"},
            {"trigger://new", "program：write 新建触发，content 为 {when, run: prog://名字?参数}"},
            {"trigger://list", "program：read 列出触发"},
        };
        for (const auto& item : items) {
            if (!add_resource(list, remaining, item[0], item[1])) return;
        }
    }
};

}  // namespace

Package* make_program_package() { return new ProgramPackage(); }

}  // namespace vat::registry
