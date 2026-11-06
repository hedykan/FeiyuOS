// web: internet search and page reading through the DeepSeek Search / Fetch
// API.  Targets:
//   read web://search?q=QUERY[&count=N]   numbered results (title, site, date, summary, url)
//   read web://page?url=URL               the page as Markdown (whole page -> out#N)
// Read-only; each request is billed by DeepSeek (Search/Fetch API).
#include "../common.h"
#include "../config.h"
#include "../ops_web.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "联网规范：用户要查网上的信息、最新消息、文档、版本、价格等你不确定或可能过时的内容时，用 read target=web://search?q=搜索词 联网搜索（搜索词不超过 128 字，可用 site: 、“短语”、after:2026 等运算符，不要把整句话原样塞进去）；需要某个结果的全文时用 read target=web://page?url=完整网址，网址放在最后。回答联网得到的内容时说明来源网站，不要编造搜索结果里没有的信息。";

class WebPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"web", "联网搜索与网页阅读（DeepSeek Search / Fetch）",
                                "联网 上网 网上 搜索 搜一下 查一下 新闻 最新 网页 链接 网址 文档 官网 web search", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "read" && json_str(arguments, "target").startsWith("web://");
    }

    String execute(const String&, const cJSON* arguments, bool) override {
        if (config::get().deepseek_key.isEmpty()) return "没有配置 DeepSeek Key，联网不可用";
        const String target = json_str(arguments, "target");
        const String path = target_path(target);
        if (path == "web://search") {
            const String count = target_param(target, "count");
            return web::search(target_param(target, "q"), count.isEmpty() ? 5 : count.toInt());
        }
        if (path == "web://page") return web::fetch(target_param(target, "url", true));
        return "web 只支持 web://search?q=… 与 web://page?url=…";
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"联网搜索", "", "搜一下 ESP32-S3 的最新 Arduino 版本"});
        out.push_back({"读网页", "", "打开第 2 个结果看看"});
    }

    void resources(cJSON* list, int& remaining) override {
        for (const char* target : {"web://search?q=", "web://page?url="}) {
            add_resource(list, remaining, target, "web");
        }
    }
};

}  // namespace

Package* make_web_package() { return new WebPackage(); }

}  // namespace vat::registry
