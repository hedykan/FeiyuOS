// place: AMap (高德) location services — where am I, nearby, search, weather,
// walking/driving/cycling directions.  Read-only; available only when the AMap
// key is set.  All network work is in ops_place.
#include "../common.h"
#include "../ops_place.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr char kUsage[] =
    "位置服务（高德，需联网）：用户问我在哪、当前位置、我在什么地方，优先 read target=place://here（返回文字地址，比 GPS 原始坐标更有用）；附近搜索 read place://nearby?q=关键词（如 place://nearby?q=咖啡）；"
    "查地点 read place://search?q=关键词（可加 &city=城市）；天气 read place://weather；"
    "路线 read place://route?to=目的地&mode=walk（mode：walk 步行 / drive 驾车 / bike 骑行 / ebike 电动车 / transit 公交地铁，默认 walk；返回文字分步与大约时间；“附近的某某”会选最近的那家）。都是只读、免确认。";

class PlacePackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    bool present() const override { return place::available(); }
    const char* index() const override { return "问“我在哪”用 place://here，另有 nearby?q=、search?q=、weather、route?to="; }

    const Manifest& manifest() const override {
        static const Manifest m{"place", "高德位置服务：当前位置、附近搜索、地点查询、天气、路线",
                                "位置 地点 在哪 附近 周边 搜索 导航 路线 怎么走 骑车 公交 地铁 天气 高德 地图 place", {}};
        return m;
    }

    bool available(const String&) override { return place::available(); }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "read" && json_str(arguments, "target").startsWith("place://");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String path = target_path(target).substring(strlen("place://"));
        if (path == "here") return place::here();
        if (path == "weather") return place::weather();
        if (path == "nearby") {
            const String q = target_param(target, "q");
            return q.isEmpty() ? "请说明要找什么（place://nearby?q=关键词）" : place::nearby(q);
        }
        if (path == "search") {
            const String q = target_param(target, "q");
            return q.isEmpty() ? "请说明要查什么（place://search?q=关键词）" : place::search(q, target_param(target, "city"));
        }
        if (path == "route") {
            const String to = target_param(target, "to");
            return to.isEmpty() ? "请说明目的地（place://route?to=…&mode=walk）"
                                : place::route(to, target_param(target, "mode"));
        }
        return "未知的 place 目标；支持 here / nearby / search / weather / route";
    }

    void hints(std::vector<Hint>& out) override {
        if (place::available()) out.push_back({"位置", "", "我现在在哪 / 附近有什么咖啡 / 今天天气"});
    }

    void resources(cJSON* list, int& remaining) override {
        if (!place::available()) return;
        static const char* const items[][2] = {
            {"place://here", "place：read 当前位置（地址）"},
            {"place://nearby?q=关键词", "place：read 附近搜索"},
            {"place://search?q=关键词", "place：read 查地点"},
            {"place://weather", "place：read 当前城市天气"},
            {"place://route?to=目的地&mode=walk", "place：read 路线（walk/drive/bike/ebike/transit，文字分步）"},
        };
        for (const auto& item : items) {
            add_resource(list, remaining, item[0], item[1]);
        }
    }
};

}  // namespace

Package* make_place_package() { return new PlacePackage(); }

}  // namespace vat::registry
