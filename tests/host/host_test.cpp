// Host tests for the portable firmware sources: run with `make host-test`.
// Each check is one line; a failure prints the expression and the run exits 1.
// Fixtures under fixtures/ are real service responses (tools/record_amap_fixtures.sh).
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "common.h"
#include "place_format.h"
#include "wifi_saved.h"

namespace {

int checks = 0;
int failures = 0;
std::string fixture_dir = "tests/host/fixtures";

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                   \
    } while (0)

// Parsed fixture; the caller deletes it.  A missing file counts as a failure.
cJSON* fixture(const char* name) {
    std::ifstream in(fixture_dir + "/" + name);
    std::stringstream text;
    text << in.rdbuf();
    cJSON* root = cJSON_Parse(text.str().c_str());
    if (!root) {
        ++failures;
        std::printf("FAIL cannot load fixture %s\n", name);
    }
    return root;
}

bool has(const String& text, const char* part) { return text.indexOf(String(part)) >= 0; }

int line_count(const String& text) {
    if (text.isEmpty()) return 0;
    int n = 1;
    for (std::size_t i = 0; i < text.length(); ++i) n += text[i] == '\n';
    return n;
}

void common_json() {
    cJSON* root = cJSON_Parse(R"({"list":[1,2],"obj":{"a":"x"},"n":"42"})");
    // The v0.5.2 bug: json_obj() is for objects only; arrays need json_arr().
    CHECK(vat::json_obj(root, "list") == nullptr);
    CHECK(vat::json_arr(root, "list") != nullptr);
    CHECK(vat::json_arr(root, "obj") == nullptr);
    CHECK(vat::json_obj(root, "obj") != nullptr);
    CHECK(vat::json_arr(nullptr, "list") == nullptr);
    CHECK(vat::json_str(vat::json_obj(root, "obj"), "a") == "x");
    CHECK(vat::json_str(root, "list", "fallback") == "fallback");  // wrong type -> fallback
    CHECK(vat::json_int(root, "n", 0) == 42);
    cJSON_Delete(root);
}

void common_text() {
    CHECK(vat::target_path("place://nearby?q=便利店") == "place://nearby");
    CHECK(vat::target_param("place://nearby?q=便利店", "q") == "便利店");
    CHECK(vat::target_param("place://route?to=深圳北站&mode=walk", "to") == "深圳北站");
    CHECK(vat::target_param("place://route?to=深圳北站&mode=walk", "mode") == "walk");
    CHECK(vat::target_param("place://here", "q").isEmpty());
    CHECK(vat::target_path("file:///vat/logs/") == "file:///vat/logs");
    CHECK(vat::utf8_head("便利店", 4) == "便");  // never cut inside a character
    CHECK(vat::utf8_tail("便利店", 4) == "店");
    CHECK(vat::utf8_sanitize(String("ok\xff")) == "ok?");
    CHECK(vat::url_decode("%E4%BE%BF") == "便");
    CHECK(vat::shell_quote("it's") == "'it'\\''s'");
    CHECK(vat::shell_quote("plain-arg") == "plain-arg");
}

void place_format() {
    using namespace vat::place;
    CHECK(fmt::url_encode("便利店") == "%E4%BE%BF%E5%88%A9%E5%BA%97");
    CHECK(fmt::url_encode("a b~") == "a%20b~");

    if (cJSON* r = fixture("amap/around.json")) {
        const String list = fmt::poi_list(r, true, 6);
        const int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "pois"));
        CHECK(n >= 3);
        CHECK(fmt::poi_count(r) == n);
        CHECK(!has(list, "没有找到"));
        CHECK(line_count(list) == std::min(n, 6));
        CHECK(has(list, "1. ") && has(list, " m）"));
        CHECK(line_count(fmt::poi_list(r, true, 2)) == 2);
        // Route destinations: nearest name match, never an unrelated nearby place.
        const int i = fmt::match_poi(r, "餐厅");
        CHECK(i >= 0 && has(fmt::poi_field(r, i, "name"), "餐厅"));
        CHECK(fmt::match_poi(r, "深圳湾公园") == -1);
        CHECK(fmt::match_poi(r, "") == -1);
        CHECK(fmt::poi_field(r, -1, "name").isEmpty());
        cJSON_Delete(r);
    }
    if (cJSON* r = fixture("amap/text.json")) {
        CHECK(has(fmt::first_poi(r, "name"), "故宫"));
        CHECK(has(fmt::first_poi(r, "location"), ","));
        CHECK(!has(fmt::poi_list(r, false, 6), "m）"));  // no distance in a plain search
        cJSON_Delete(r);
    }
    if (cJSON* r = fixture("amap/regeo.json")) {
        CHECK(has(fmt::regeo_address(r), "北京"));
        CHECK(fmt::regeo_adcode(r).length() == 6);
        CHECK(fmt::regeo_citycode(r) == "010");
        cJSON_Delete(r);
    }
    CHECK(fmt::duration_text(0).isEmpty());
    CHECK(fmt::duration_text(59) == "约 1 分钟");
    CHECK(fmt::duration_text(3600) == "约 1 小时");
    CHECK(fmt::duration_text(3900) == "约 1 小时 5 分钟");
    if (cJSON* r = fixture("amap/weather.json")) {
        const String w = fmt::weather_text(r);
        CHECK(has(w, "℃") && has(w, "湿度"));
        cJSON_Delete(r);
    }
    for (const char* name : {"amap/walking.json", "amap/driving.json", "amap/bicycling.json"}) {
        if (cJSON* r = fixture(name)) {
            const String route = fmt::route_text(r, "故宫");
            CHECK(route.startsWith("到故宫（"));
            CHECK(has(route, " m，约 ") && has(route, "分钟）："));  // duration from cost or path
            CHECK(has(route, "\n1. "));
            CHECK(line_count(route) >= 3);
            CHECK(line_count(fmt::route_text(r, "故宫", 1)) <= 3);  // header, one step, "…"
            cJSON_Delete(r);
        }
    }
    if (cJSON* r = fixture("amap/transit.json")) {
        const String t = fmt::transit_text(r, "望京");
        CHECK(t.startsWith("到望京（公交约 "));
        CHECK(has(t, "票价") && has(t, "步行"));
        CHECK(has(t, "\n1. 步行 ") && has(t, ". 乘 ") && has(t, " → ") && has(t, " 站）"));
        CHECK(has(t, "到达目的地"));
        cJSON_Delete(r);
    }
    if (cJSON* r = fixture("amap/ip.json")) {
        String adcode;
        const String point = fmt::ip_center(r, adcode);
        CHECK(has(point, ","));
        CHECK(point.substring(0, point.indexOf(',')).toDouble() > 115.0);  // Beijing longitude
        CHECK(adcode.length() == 6);
        cJSON_Delete(r);
    }

    // Shapes AMap sends when it finds nothing.
    cJSON* empty = cJSON_Parse(R"({"status":"1","count":"0","pois":[],"route":{"paths":[]},"lives":[],
                                   "rectangle":[],"adcode":[],"regeocode":{"formatted_address":[]}})");
    String adcode = "x";
    CHECK(fmt::poi_list(empty, true, 6) == "没有找到");
    CHECK(fmt::poi_count(empty) == 0);
    CHECK(fmt::first_poi(empty, "location").isEmpty());
    CHECK(fmt::route_text(empty, "x") == "没有找到路线");
    CHECK(fmt::transit_text(empty, "x") == "没有找到公交方案");
    CHECK(fmt::weather_text(empty) == "没有天气数据");
    CHECK(fmt::regeo_address(empty).isEmpty());
    CHECK(fmt::ip_center(empty, adcode).isEmpty() && adcode.isEmpty());
    cJSON_Delete(empty);
}

// Saved password of `ssid` in the list, "" when not listed.
String saved_password(const cJSON* wifi, const char* ssid) {
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, vat::json_arr(wifi, "networks")) {
        if (vat::json_str(item, "ssid") == ssid) return vat::json_str(item, "password");
    }
    return "";
}

void wifi_saved() {
    // Old setups wrote the primary network alone, with no list.
    cJSON* wifi = cJSON_Parse(R"({"ssid":"home","password":"pw-home"})");
    vat::wifi_saved::remember(wifi, "hotspot", "pw-hot");
    CHECK(vat::json_str(wifi, "ssid") == "hotspot");             // new primary
    CHECK(saved_password(wifi, "home") == "pw-home");             // the bug: home was forgotten here
    CHECK(saved_password(wifi, "hotspot") == "pw-hot");
    vat::wifi_saved::remember(wifi, "home", "pw-home");
    CHECK(vat::json_str(wifi, "ssid") == "home");
    CHECK(cJSON_GetArraySize(vat::json_arr(wifi, "networks")) == 2);  // no duplicates
    CHECK(saved_password(wifi, "hotspot") == "pw-hot");
    vat::wifi_saved::remember(wifi, "home", "pw-new");            // a changed password replaces the old one
    CHECK(saved_password(wifi, "home") == "pw-new" && vat::json_str(wifi, "password") == "pw-new");
    CHECK(cJSON_GetArraySize(vat::json_arr(wifi, "networks")) == 2);
    cJSON_Delete(wifi);

    cJSON* fresh = cJSON_CreateObject();  // first network ever
    vat::wifi_saved::remember(fresh, "home", "pw-home");
    CHECK(vat::json_str(fresh, "ssid") == "home" && cJSON_GetArraySize(vat::json_arr(fresh, "networks")) == 1);
    cJSON_Delete(fresh);
}

}  // namespace

#include "prog_test.cpp"
#include "ac_test.cpp"
#include "arena_test.cpp"
#include "ui_panel_test.cpp"
#include "web_token_test.cpp"
#include "ui_canvas_test.cpp"

int main(int argc, char** argv) {
    if (argc > 1) fixture_dir = argv[1];
    common_json();
    common_text();
    place_format();
    wifi_saved();
    prog_engine();
    ac_core();
    arena_ledger();
    ui_panel();
    web_token();
    ui_canvas();
    std::printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
