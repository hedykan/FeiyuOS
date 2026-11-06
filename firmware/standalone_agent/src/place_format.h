#pragma once

#include "common.h"

// AMap (高德) responses -> the short text the place package returns.  Pure
// functions over parsed JSON with no I/O, so they also build on the host
// (tests/host) and are checked against recorded responses there.
namespace vat::place::fmt {

String url_encode(const String& text);  // RFC 3986 unreserved kept, the rest %XX

int poi_count(const cJSON* root);                            // entries in "pois"
String poi_list(const cJSON* root, bool with_distance, int limit);  // numbered list, "没有找到" when empty
String poi_field(const cJSON* root, int index, const char* field);  // pois[index].field, "" when absent
String first_poi(const cJSON* root, const char* field);      // pois[0].field
// First of "pois" (nearest first in an around search) whose name contains
// `keyword`, or -1: "附近的美宜佳" goes to the closest 美宜佳, while a named
// place that is not nearby ("深圳湾公园") is not mistaken for some local park.
int match_poi(const cJSON* root, const String& keyword);

String regeo_address(const cJSON* root);   // regeocode.formatted_address, "" when absent
String regeo_adcode(const cJSON* root);    // regeocode.addressComponent.adcode, "" when absent
String regeo_citycode(const cJSON* root);  // regeocode.addressComponent.citycode (transit wants it)

String weather_text(const cJSON* root);   // lives[0], "没有天气数据" when absent

String duration_text(long seconds);  // "约 12 分钟" / "约 1 小时 5 分钟", "" when unknown

// v5 walking / driving / bicycling / electrobike: route.paths[0] as
// "到<dest>（N m，约 M 分钟）：" plus numbered steps, "没有找到路线" when absent.
String route_text(const cJSON* root, const String& dest_name, int max_steps = 12);

// v5 transit: route.transits[0] as a header (time, distance, walking, fare)
// plus one line per walk or ride, "没有找到公交方案" when absent.
String transit_text(const cJSON* root, const String& dest_name);

// Centre of the city rectangle of an IP-location answer as "lon,lat" ("" when
// AMap does not know the address); `adcode` gets the district code.
String ip_center(const cJSON* root, String& adcode);

}  // namespace vat::place::fmt
