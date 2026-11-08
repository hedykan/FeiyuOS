// gps: the GNSS receiver on the Cap LoRa-1262 (receive only).  It follows the
// sensor convention of the prompt, sensor://NAME/METRIC:
//   read    sensor://gps/location|satellites|time   (any metric returns the full status)
//   read    sensor://gps/address                    road, district, city (OpenStreetMap), for nearby searches
//   session connect sensor://gps       start reading (default on)
//   session disconnect sensor://gps    stop reading (the cap keeps the module powered)
#include "../common.h"
#include "../ops_gps.h"
#include "../ops_lora.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "定位规范：用户问我在哪、当前位置、经纬度、海拔时用 read target=sensor://gps/location；卫星数用 sensor://gps/satellites，卫星时间用 sensor://gps/time。";

class GpsPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"gps", "Cap LoRa-1262 上的 GPS/北斗定位：位置、卫星、时间",
                                "gps GPS 北斗 定位 位置 坐标 经纬度 海拔 卫星 我在哪 现在在哪 地址 街道 附近 周边", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        String target = json_str(arguments, "target");
        target.toLowerCase();
        if (!(target == "sensor://gps" || target.startsWith("sensor://gps/"))) return false;
        return tool == "read" || tool == "session";
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        if (tool == "session") {
            const String action = json_str(arguments, "action");
            if (action == "connect" || action == "attach" || action == "create") return gps::enable(true);
            if (action == "disconnect") return gps::enable(false);
        }
        String target = json_str(arguments, "target");
        target.toLowerCase();
        if (target.endsWith("/address")) return gps::address_text();
        return gps::status_text();
    }

    void hints(std::vector<Hint>& out) override {
        if (!lora::cap_present()) return;
        const String status = gps::status_text();
        out.push_back({"GPS", status.substring(0, status.indexOf('\n')), "我现在在哪"});
        out.push_back({"附近", "", "附近有什么咖啡店"});
    }

    void streams(std::vector<StreamSpec>& out) const override {
        if (!lora::cap_present()) return;
        out.push_back({"sensor://gps/position", "lat lon speed sats", "", 1000,
                       "当前位置：纬度、经度（度）、速度（km/h）、在用卫星数；没定位时取不到"});
    }

    String open_stream(const String&) override {
        return gps::active() ? String() : String("GPS 没有打开（说「打开 GPS」）");
    }

    bool sample(const String&, StreamValue& out) override {
        double lat, lon, speed;
        int sats;
        if (!gps::fix(lat, lon, speed, sats)) return false;
        out.v[0] = lat;
        out.v[1] = lon;
        out.v[2] = speed;
        out.v[3] = sats;
        out.count = 4;
        return true;
    }

    void resources(cJSON* list, int& remaining) override {
        if (!lora::cap_present()) return;
        if (!add_resource(list, remaining, "sensor://gps/location", "gps")) return;
        add_resource(list, remaining, "sensor://gps/address", "gps：地址（搜索附近的地方先读它）");
    }
};

}  // namespace

Package* make_gps_package() { return new GpsPackage(); }

}  // namespace vat::registry
