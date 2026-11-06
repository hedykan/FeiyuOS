#include "place_format.h"

namespace vat::place::fmt {

String url_encode(const String& text) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    for (std::size_t i = 0; i < text.length(); ++i) {
        const unsigned char c = text[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

int poi_count(const cJSON* root) { return cJSON_GetArraySize(json_arr(root, "pois")); }

String poi_list(const cJSON* root, bool with_distance, int limit) {
    String out;
    int n = 0;
    const cJSON* p = nullptr;
    cJSON_ArrayForEach(p, json_arr(root, "pois")) {
        if (n >= limit) break;
        const String name = json_str(p, "name");
        if (name.isEmpty()) continue;
        out += String(++n) + ". " + name;
        const String addr = json_str(p, "address");  // AMap sends [] for an unknown address
        if (!addr.isEmpty()) out += " — " + addr;
        if (with_distance) {
            const String d = json_str(p, "distance");
            if (!d.isEmpty()) out += "（" + d + " m）";
        }
        out += "\n";
    }
    out.trim();
    return out.isEmpty() ? String("没有找到") : out;
}

String poi_field(const cJSON* root, int index, const char* field) {
    return index < 0 ? String() : json_str(cJSON_GetArrayItem(json_arr(root, "pois"), index), field);
}

String first_poi(const cJSON* root, const char* field) { return poi_field(root, 0, field); }

int match_poi(const cJSON* root, const String& keyword) {
    int i = 0;
    const cJSON* p = nullptr;
    cJSON_ArrayForEach(p, json_arr(root, "pois")) {
        if (!keyword.isEmpty() && json_str(p, "name").indexOf(keyword) >= 0) return i;
        ++i;
    }
    return -1;
}

String regeo_address(const cJSON* root) { return json_str(json_obj(root, "regeocode"), "formatted_address"); }

String regeo_adcode(const cJSON* root) {
    return json_str(json_obj(json_obj(root, "regeocode"), "addressComponent"), "adcode");
}

String regeo_citycode(const cJSON* root) {
    return json_str(json_obj(json_obj(root, "regeocode"), "addressComponent"), "citycode");
}

String duration_text(long seconds) {
    if (seconds <= 0) return "";
    const long minutes = (seconds + 59) / 60;
    if (minutes < 60) return "约 " + String(minutes) + " 分钟";
    return "约 " + String(minutes / 60) + " 小时" + (minutes % 60 ? " " + String(minutes % 60) + " 分钟" : String());
}

String weather_text(const cJSON* root) {
    const cJSON* l = cJSON_GetArrayItem(json_arr(root, "lives"), 0);
    if (!l) return "没有天气数据";
    return json_str(l, "city") + "：" + json_str(l, "weather") + " " + json_str(l, "temperature") + "℃，" +
           json_str(l, "winddirection") + "风 " + json_str(l, "windpower") + "，湿度 " + json_str(l, "humidity") +
           "%";
}

String route_text(const cJSON* root, const String& dest_name, int max_steps) {
    const cJSON* path0 = cJSON_GetArrayItem(json_arr(json_obj(root, "route"), "paths"), 0);
    if (!path0) return "没有找到路线";
    // Cycling carries its own duration; walking and driving only with show_fields=cost.
    long seconds = json_int(path0, "duration", 0);
    if (seconds <= 0) seconds = json_int(json_obj(path0, "cost"), "duration", 0);
    const String time = duration_text(seconds);
    String out = "到" + dest_name + "（" + json_str(path0, "distance") + " m" + (time.isEmpty() ? "" : "，" + time) +
                 "）：\n";
    int n = 0;
    const cJSON* s = nullptr;
    cJSON_ArrayForEach(s, json_arr(path0, "steps")) {
        if (n >= max_steps) {
            out += "…";
            break;
        }
        const String ins = json_str(s, "instruction");
        if (!ins.isEmpty()) out += String(++n) + ". " + ins + "\n";
    }
    out.trim();
    return out;
}

String transit_text(const cJSON* root, const String& dest_name) {
    const cJSON* t = cJSON_GetArrayItem(json_arr(json_obj(root, "route"), "transits"), 0);
    if (!t) return "没有找到公交方案";
    const cJSON* cost = json_obj(t, "cost");
    const String time = duration_text(json_int(cost, "duration", 0));
    const String fee = json_str(cost, "transit_fee");
    String out = "到" + dest_name + "（公交" + (time.isEmpty() ? "" : time + "，") + "全程 " +
                 json_str(t, "distance") + " m，步行 " + json_str(t, "walking_distance") + " m" +
                 (fee.isEmpty() ? "" : "，票价 " + fee + " 元") + "）：\n";
    int n = 0;
    const cJSON* seg = nullptr;
    const cJSON* segments = json_arr(t, "segments");
    const int last = cJSON_GetArraySize(segments) - 1;
    int index = 0;
    cJSON_ArrayForEach(seg, segments) {
        const cJSON* line = cJSON_GetArrayItem(json_arr(json_obj(seg, "bus"), "buslines"), 0);
        const String from = json_str(json_obj(line, "departure_stop"), "name");
        const long walk = json_int(json_obj(seg, "walking"), "distance", 0);
        if (walk > 0) {
            out += String(++n) + ". 步行 " + String(walk) + " m" +
                   (!from.isEmpty() ? " 到 " + from : index == last ? String(" 到达目的地") : String()) + "\n";
        }
        if (line) {
            const long stops = json_int(line, "via_num", 0) + 1;
            out += String(++n) + ". 乘 " + json_str(line, "name") + "：" + from + " → " +
                   json_str(json_obj(line, "arrival_stop"), "name") + "（" + String(stops) + " 站）\n";
        }
        const cJSON* rail = json_obj(seg, "railway");
        if (rail && !json_str(rail, "name").isEmpty()) {
            out += String(++n) + ". 乘 " + json_str(rail, "name") + "：" +
                   json_str(json_obj(rail, "departure_stop"), "name") + " → " +
                   json_str(json_obj(rail, "arrival_stop"), "name") + "\n";
        }
        ++index;
    }
    out.trim();
    return out;
}

String ip_center(const cJSON* root, String& adcode) {
    adcode = json_str(root, "adcode");
    const String rect = json_str(root, "rectangle");  // "lon1,lat1;lon2,lat2", [] when unknown
    const int semi = rect.indexOf(';');
    if (semi < 0) return "";
    const String a = rect.substring(0, semi), b = rect.substring(semi + 1);
    const int ca = a.indexOf(','), cb = b.indexOf(',');
    if (ca < 0 || cb < 0) return "";
    const double lon = (a.substring(0, ca).toDouble() + b.substring(0, cb).toDouble()) / 2;
    const double lat = (a.substring(ca + 1).toDouble() + b.substring(cb + 1).toDouble()) / 2;
    return String(lon, 6) + "," + String(lat, 6);
}

}  // namespace vat::place::fmt
