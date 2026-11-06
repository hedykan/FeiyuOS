// memory: the long context on the TF card — complete tool outputs (out#N),
// the searchable archive of past tasks, and cases for investigations that
// span many tasks.  Targets:
//   read  memory://out                     recent complete outputs (numbered)
//   read  memory://out/N[?page=P][&grep=W] one page (4 KB) or matching lines
//   read  memory://history?q=W             past steps and answers matching W
//   read  memory://cases[/NAME]            case list, or one case
//   write memory://cases/NAME              mode=append: a finding; overwrite: the notes
//   session attach|create memory://cases/NAME   make it the current case
//   session disconnect memory://cases[/NAME]    close the current case
#include "../common.h"
#include "../context.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// "memory://cases/x" after "memory://cases" -> "x"
String after(const String& path, const char* prefix) {
    const String rest = path.substring(strlen(prefix));
    return rest.startsWith("/") ? rest.substring(1) : rest;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "长上下文规范：工具结果末尾的“完整输出 out#N”说明全文存在卡上，按其中提示用 memory://out/N 翻页或搜索。用户问以前做过什么、上次的结果时，先看对话和摘要，没有再用 read target=memory://history?q=关键词 检索。持续多次的排查、渗透测试等调查用案卷：用户要开始或继续某项调查时 session(action=attach,target=memory://cases/名称)，结束时 session(action=disconnect,target=memory://cases/名称)；有案卷时，把确认的发现用 write target=memory://cases/名称 mode=append 记录，引用相关的 out#N；列出案卷用 read target=memory://cases。";

class MemoryPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    bool reversible(const String&, const cJSON* arguments) const override {
        return json_str(arguments, "target").startsWith("memory://cases/");
    }

    const Manifest& manifest() const override {
        static const Manifest m{"memory", "TF 卡长上下文：完整输出 out#N 分页与搜索、历史检索、案卷",
                                "完整输出 输出 out 分页 翻页 grep 搜索 历史 以前 上次 检索 案卷 调查 排查 渗透 发现 记录",
                                {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (!context::owns_target(target)) return false;
        if (tool == "read") return true;
        if (tool == "write") return target.startsWith("memory://cases/");
        if (tool == "session") return target.startsWith("memory://cases");
        return false;
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String raw = json_str(arguments, "target");
        const String path = target_path(raw);
        if (tool == "session") {
            const String action = json_str(arguments, "action");
            const String name = after(path, "memory://cases");
            if (action == "attach" || action == "create" || action == "connect") return context::attach_case(name);
            if (action == "disconnect") return context::detach_case(name);
            if (action == "list") return context::list_cases();
            return "案卷只支持 attach / disconnect / list";
        }
        if (tool == "write") {
            const bool append = json_str(arguments, "mode", "append") != "overwrite";
            return context::write_case(after(path, "memory://cases"), json_str(arguments, "content"), append);
        }
        if (path == "memory://out") return context::list_outputs();
        if (path.startsWith("memory://out/")) {
            const long id = after(path, "memory://out").toInt();
            if (id <= 0) return "out 编号无效：" + path;
            const String page = target_param(raw, "page");
            return context::read_output(id, page.isEmpty() ? 1 : page.toInt(), target_param(raw, "grep"));
        }
        if (path == "memory://history") {
            String query = target_param(raw, "q");
            if (query.isEmpty()) query = json_str(arguments, "query");
            return context::search_history(query);
        }
        if (path == "memory://cases") return context::list_cases();
        return context::read_case(after(path, "memory://cases"));
    }

    void hints(std::vector<Hint>& out) override {
        if (!context::on_card()) return;
        const String current = context::current_case();
        out.push_back({"案卷", current.isEmpty() ? String("无") : current, "开一个叫内网排查的案卷"});
        out.push_back({"长输出", "", "刚才那个日志里有没有 ERROR"});
        out.push_back({"历史", "", "我之前在哪台机器上看过磁盘"});
    }

    void resources(cJSON* list, int& remaining) override {
        if (!context::on_card()) return;
        for (const char* target : {"memory://out", "memory://history", "memory://cases"}) {
            add_resource(list, remaining, target, "memory");
        }
    }
};

}  // namespace

Package* make_memory_package() { return new MemoryPackage(); }

}  // namespace vat::registry
