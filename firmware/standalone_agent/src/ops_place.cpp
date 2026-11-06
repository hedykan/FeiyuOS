#include "ops_place.h"

#include "common.h"
#include "config.h"
#include "net.h"
#include "ops_gps.h"
#include "place_format.h"

namespace vat::place {
namespace {

// Search, geocoding, weather and IP location are v3; route planning is v5
// ("路径规划 2.0"): one response shape for every mode, and much smaller answers.
constexpr char kBase[] = "https://restapi.amap.com/";
constexpr uint32_t kTimeoutMs = 12000;
// Set by current_point(): the last position came from IP location (a city
// centre), so results around it must say they are approximate.
bool approximate = false;
constexpr char kApproximate[] = "（没有 GPS 定位，按 IP 估算的城市中心，位置可能不准）\n";

// GET base + path (path already carries its query, without the key).  Returns
// the parsed object with status=="1", or nullptr; `err` gets a short reason.
cJSON* call(const String& path, String& err) {
    const auto& cfg = config::get();
    if (cfg.amap_key.isEmpty()) {
        err = "未设置高德 Key（设置页填写）";
        return nullptr;
    }
    const String sep = path.indexOf('?') >= 0 ? "&" : "?";
    // AMap occasionally answers with an empty body; retry a few times.
    cJSON* root = nullptr;
    for (int attempt = 0; attempt < 3 && !root; ++attempt) {
        if (attempt) delay(300);
        const net::HttpResult res = net::get(kBase + path + sep + "key=" + cfg.amap_key, "", kTimeoutMs);
        if (!res.ok()) {
            err = "高德请求失败 HTTP " + String(res.status);
            continue;
        }
        root = cJSON_Parse(res.body.c_str());
        if (!root) err = "高德返回为空或无法解析";
    }
    if (!root) return nullptr;
    if (json_str(root, "status") != "1") {
        err = "高德：" + json_str(root, "info", "未知错误");
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

// Current position as "lon,lat" in GCJ-02 (what every other AMap call wants).
// GPS fix (WGS-84) converted by AMap; IP location when there is no fix.
// `adcode` (district code) is filled when available (IP path), else empty.
String current_point(String& adcode, String& err) {
    double lat = 0, lon = 0;
    approximate = false;
    if (gps::location(lat, lon)) {
        const String raw = String(lon, 6) + "," + String(lat, 6);
        String e2;
        if (cJSON* conv = call("v3/assistant/coordinate/convert?coordsys=gps&locations=" + raw, e2)) {
            const String locs = json_str(conv, "locations");
            cJSON_Delete(conv);
            if (!locs.isEmpty()) return locs;
        }
        return raw;  // conversion failed: the raw fix is a few hundred metres off
    }
    approximate = true;
    cJSON* ip = call("v3/ip", err);
    if (!ip) return "";
    const String point = fmt::ip_center(ip, adcode);
    cJSON_Delete(ip);
    if (point.isEmpty()) err = "无法定位（无 GPS 信号，IP 定位也失败）";
    return point;
}

// District code (adcode) or city code (citycode) of `point` through reverse
// geocoding, "" on failure.
String code_of(const String& point, bool city, String& err) {
    cJSON* r = call("v3/geocode/regeo?location=" + point, err);
    if (!r) return "";
    const String code = city ? fmt::regeo_citycode(r) : fmt::regeo_adcode(r);
    cJSON_Delete(r);
    return code;
}

struct Mode {
    const char* name;
    const char* path;
    const char* extra;
};

// v5 endpoints.  Cycling answers carry their duration; the others need show_fields=cost.
constexpr Mode kModes[] = {
    {"walk", "walking", "&show_fields=cost"},
    {"drive", "driving", "&strategy=0&show_fields=cost"},  // strategy 0: the fastest route only
    {"bike", "bicycling", ""},
    {"ebike", "electrobike", ""},
    {"transit", "transit/integrated", "&AlternativeRoute=1&show_fields=cost"},  // one plan
};

}  // namespace

bool available() { return !config::get().amap_key.isEmpty(); }

String here() {
    String adcode, err;
    const String point = current_point(adcode, err);
    if (point.isEmpty()) return err;
    cJSON* r = call("v3/geocode/regeo?location=" + point, err);
    if (!r) return err;
    const String address = fmt::regeo_address(r);
    cJSON_Delete(r);
    return (approximate ? String(kApproximate) : String()) + "当前位置：" + (address.isEmpty() ? String("未知") : address);
}

String nearby(const String& keyword) {
    String adcode, err;
    const String point = current_point(adcode, err);
    if (point.isEmpty()) return err;
    cJSON* r = call("v3/place/around?radius=2000&offset=6&location=" + point + "&keywords=" + fmt::url_encode(keyword),
                    err);
    if (!r) return err;
    const String out = (approximate ? String(kApproximate) : String()) + "附近的" + keyword + "：\n" + fmt::poi_list(r, true, 6);
    cJSON_Delete(r);
    return out;
}

String search(const String& keyword, const String& city) {
    String path = "v3/place/text?offset=6&keywords=" + fmt::url_encode(keyword);
    if (!city.isEmpty()) path += "&city=" + fmt::url_encode(city);
    String err;
    cJSON* r = call(path, err);
    if (!r) return err;
    const String out = fmt::poi_list(r, false, 6);
    cJSON_Delete(r);
    return out;
}

String weather() {
    String adcode, err;
    const String point = current_point(adcode, err);
    if (point.isEmpty()) return err;
    if (adcode.isEmpty()) adcode = code_of(point, false, err);  // GPS path has no adcode
    if (adcode.isEmpty()) return err.isEmpty() ? String("无法确定所在城市") : err;
    cJSON* w = call("v3/weather/weatherInfo?city=" + adcode, err);
    if (!w) return err;
    const String out = fmt::weather_text(w);
    cJSON_Delete(w);
    return out;
}

String route(const String& destination, const String& mode) {
    const Mode* m = &kModes[0];  // walking unless another mode is named
    for (const auto& candidate : kModes) {
        if (mode == candidate.name) m = &candidate;
    }
    String adcode, err;
    const String origin = current_point(adcode, err);
    if (origin.isEmpty()) return err;

    // The destination: the nearest match within 3 km first ("附近的美宜佳"),
    // then the current city, then anywhere.
    String dest_loc, dest_name;
    const String keyword = fmt::url_encode(destination);
    // The origin's city code, looked up once when first needed (the in-city
    // search and transit).  Not the district code: that confines the search to
    // one district, and "深圳湾公园" asked from 龙华 became 观澜海洋世界.
    String city;
    const auto city_code = [&]() {
        if (city.isEmpty()) {
            String e2;
            city = code_of(origin, true, e2);
        }
        return city;
    };
    if (cJSON* r = call("v3/place/around?radius=3000&offset=5&sortrule=distance&location=" + origin +
                            "&keywords=" + keyword,
                        err)) {
        const int i = fmt::match_poi(r, destination);
        dest_loc = fmt::poi_field(r, i, "location");
        dest_name = fmt::poi_field(r, i, "name");
        cJSON_Delete(r);
    }
    if (dest_loc.isEmpty()) {
        const String query = "v3/place/text?offset=1&keywords=" + keyword;
        cJSON* found = city_code().isEmpty() ? nullptr : call(query + "&citylimit=true&city=" + city, err);
        if (found && fmt::poi_count(found) == 0) {
            cJSON_Delete(found);
            found = nullptr;
        }
        if (!found) found = call(query, err);
        if (!found) return err;
        dest_loc = fmt::first_poi(found, "location");
        dest_name = fmt::first_poi(found, "name");
        cJSON_Delete(found);
    }
    if (dest_loc.isEmpty()) return "没有找到目的地「" + destination + "」";
    if (dest_name.isEmpty()) dest_name = destination;

    const bool transit = String(m->name) == "transit";
    String path = "v5/direction/" + String(m->path) + "?origin=" + origin + "&destination=" + dest_loc + m->extra;
    if (transit) {  // transit wants both ends' city codes
        const String from = city_code();
        const String to = code_of(dest_loc, true, err);
        if (from.isEmpty() || to.isEmpty()) return err.isEmpty() ? String("无法确定所在城市，公交路线查不了") : err;
        path += "&city1=" + from + "&city2=" + to;
    }
    cJSON* r = call(path, err);
    if (!r) return err;
    const String out = (approximate ? String(kApproximate) : String()) +
                       (transit ? fmt::transit_text(r, dest_name) : fmt::route_text(r, dest_name));
    cJSON_Delete(r);
    return out;
}

}  // namespace vat::place
