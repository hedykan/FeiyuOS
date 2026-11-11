// debug: what programs read, printed and got wrong, run by run.  The three
// global streams are written by prog_runtime after every run (io_log).
//   read debug://stdout | debug://stderr | debug://stdin   [?n=5&prog=名字&failed=1]
//   read debug://run/编号
// Memory is not here: it is part of device://local/status.
#include "../common.h"
#include "../io_log.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt.  JSON-safe text only (no double quotes or backslashes).
constexpr char kUsage[] =
    "调试（debug://）：程序每次运行（前台、定时、触发）的输入、输出、错误都记在三条全局的流里，按运行编号对应。流用来查之前的运行；"
    "你刚运行的结果已经在工具结果里，不用再读一遍。read debug://stdout（每次运行的记录和输出）、debug://stderr（错误输出）、"
    "debug://stdin（输入），可带 ?n=最近几次&prog=程序名&failed=1（只看失败的）；read debug://run/编号 看一次运行的全部。"
    "内存在 read device://local/status 里。";

// "n=3&prog=名字" -> the value of `key` (decoded), "" when absent.
String param(const String& query, const char* key) {
    const String want = String(key) + "=";
    int at = 0;
    while (at < static_cast<int>(query.length())) {
        int end = query.indexOf('&', at);
        if (end < 0) end = query.length();
        const String pair = query.substring(at, end);
        if (pair.startsWith(want)) return url_decode(pair.substring(want.length()));
        at = end + 1;
    }
    return {};
}

class DebugPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"debug", "调试：程序每次运行的输入、输出、错误（三条全局的流）",
                                "调试 debug 日志 记录 输出 错误 失败 报错 上次 刚才 昨天 昨晚 运行 历史 为什么 stdout stderr stdin log",
                                {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "read" && json_str(arguments, "target").startsWith("debug://");
    }

    String execute(const String&, const cJSON* arguments, bool) override {
        const String rest = json_str(arguments, "target").substring(strlen("debug://"));
        const int mark = rest.indexOf('?');
        String path = mark < 0 ? rest : rest.substring(0, mark);
        const String query = mark < 0 ? String() : rest.substring(mark + 1);
        while (path.endsWith("/")) path.remove(path.length() - 1);
        if (path.startsWith("run/")) return iolog::read_run(path.substring(4).toInt());
        const int count = param(query, "n").toInt();
        return iolog::read(path, count > 0 ? count : 5, param(query, "prog"), param(query, "failed") == "1");
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"运行记录", "", "昨晚的定时程序为什么失败了"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"debug://stdout", "debug：read 所有程序最近几次运行的记录和输出（?n= ?prog= ?failed=1）"},
            {"debug://stderr", "debug：read 最近几次运行的错误输出"},
            {"debug://stdin", "debug：read 最近几次运行读到的输入"},
            {"debug://run/编号", "debug：read 一次运行的输入、输出、错误"},
        };
        for (const auto& item : items) {
            if (!add_resource(list, remaining, item[0], item[1])) return;
        }
    }
};

}  // namespace

Package* make_debug_package() { return new DebugPackage(); }

}  // namespace vat::registry
