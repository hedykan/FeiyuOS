#include "ac_core.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "third_party/irext/ir_decode.h"
}

namespace vat::ac {
namespace {

constexpr const char* kModeLabels[kModeCount] = {"制冷", "制热", "自动", "送风", "除湿"};
constexpr const char* kFanLabels[kFanCount] = {"自动风", "低风", "中风", "高风"};

bool is_one_of(const String& word, std::initializer_list<const char*> list) {
    for (const char* w : list) {
        if (word == w) return true;
    }
    return false;
}

// Splits on spaces and the separators people (and the model) type.
std::vector<String> tokens(String text) {
    for (const char* sep : {"，", "、", "；", ",", ";", "="}) {
        int at;
        while ((at = text.indexOf(sep)) >= 0) {
            text = text.substring(0, at) + " " + text.substring(at + strlen(sep));
        }
    }
    std::vector<String> out;
    String word;
    for (std::size_t i = 0; i <= text.length(); ++i) {
        const char c = i < text.length() ? text[i] : ' ';
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!word.isEmpty()) out.push_back(word);
            word = "";
        } else {
            word += c;
        }
    }
    return out;
}

int mode_word(const String& w) {
    if (is_one_of(w, {"cool", "制冷", "冷", "冷风"})) return kCool;
    if (is_one_of(w, {"heat", "制热", "热", "暖", "暖风"})) return kHeat;
    if (is_one_of(w, {"自动模式"})) return kAuto;
    if (is_one_of(w, {"fan_only", "送风", "通风"})) return kFan;
    if (is_one_of(w, {"dry", "除湿", "抽湿"})) return kDry;
    return -1;
}

int fan_word(const String& w) {
    if (is_one_of(w, {"自动风", "风速自动"})) return kFanAuto;
    if (is_one_of(w, {"low", "低风", "小风", "低速"})) return kFanLow;
    if (is_one_of(w, {"mid", "medium", "中风", "中速"})) return kFanMid;
    if (is_one_of(w, {"high", "高风", "大风", "高速"})) return kFanHigh;
    return -1;
}

// "26", "26度", "26°C", "26℃"; 0 when not a temperature.
int temp_word(String w) {
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const char* unit : {"℃", "°", "度", "c", "C"}) {
            if (w.endsWith(unit)) {
                w = w.substring(0, w.length() - strlen(unit));
                stripped = true;
            }
        }
    }
    if (w.isEmpty()) return 0;
    for (std::size_t i = 0; i < w.length(); ++i) {
        if (w[i] < '0' || w[i] > '9') return 0;
    }
    return static_cast<int>(w.toInt());
}

// "+1", "-2", "up", "调高" …; 0 when not a step.
int step_word(const String& w) {
    if (is_one_of(w, {"up", "调高", "升温", "高一度", "加一度"})) return 1;
    if (is_one_of(w, {"down", "调低", "降温", "低一度", "减一度"})) return -1;
    if (w.length() >= 2 && (w[0] == '+' || w[0] == '-')) {
        const int n = temp_word(w.substring(1));
        if (n > 0 && n <= 10) return w[0] == '+' ? n : -n;
    }
    return 0;
}

}  // namespace

const char* mode_label(uint8_t mode) { return mode < kModeCount ? kModeLabels[mode] : "?"; }
const char* fan_label(uint8_t fan) { return fan < kFanCount ? kFanLabels[fan] : "?"; }

String describe(const State& s) {
    if (!s.on) return "关";
    return String("开，") + mode_label(s.mode) + " " + String(s.temp) + "°C，" + fan_label(s.fan);
}

bool apply(const String& words, State& state, String& brand, String& error) {
    State next = state;
    bool power_word = false, setting_word = false;
    String pending;  // "mode" / "fan" / "temp" / "power" / "brand" after a key= word
    for (String w : tokens(words)) {
        String lower = w;
        lower.toLowerCase();
        if (is_one_of(lower, {"mode", "fan", "temp", "power", "brand", "模式", "风速", "温度", "品牌"})) {
            pending = lower;
            continue;
        }
        const String key = pending;
        pending = "";
        if (key == "brand" || key == "品牌") {
            if (!brand.isEmpty() && brand != w) {
                error = "只能有一个品牌：「" + brand + "」和「" + w + "」";
                return false;
            }
            brand = w;
        } else if ((key.isEmpty() || key == "power") && is_one_of(lower, {"on", "开", "开机", "打开", "启动"})) {
            next.on = true;
            power_word = true;
        } else if ((key.isEmpty() || key == "power") && is_one_of(lower, {"off", "关", "关机", "关闭", "关掉"})) {
            next.on = false;
            power_word = true;
        } else if ((key == "mode" || key == "模式") && lower == "auto") {
            next.mode = kAuto;
            setting_word = true;
        } else if ((key == "fan" || key == "风速") && lower == "auto") {
            next.fan = kFanAuto;
            setting_word = true;
        } else if (mode_word(lower) >= 0) {
            next.mode = static_cast<uint8_t>(mode_word(lower));
            setting_word = true;
        } else if (fan_word(lower) >= 0) {
            next.fan = static_cast<uint8_t>(fan_word(lower));
            setting_word = true;
        } else if (step_word(lower) != 0) {
            next.temp += step_word(lower);
            setting_word = true;
        } else if (temp_word(lower) > 0) {
            const int t = temp_word(lower);
            if (t < kTempMin || t > kTempMax) {
                error = "温度要在 " + String(kTempMin) + "–" + String(kTempMax) + "°C 之间：「" + w + "」";
                return false;
            }
            next.temp = t;
            setting_word = true;
        } else if (lower == "auto") {
            error = "「auto」要写明是模式还是风速：mode=auto 或 fan=auto";
            return false;
        } else if (brand.isEmpty() || brand == w) {
            brand = w;
        } else {
            error = "看不懂「" + w + "」（已把「" + brand + "」当作品牌）";
            return false;
        }
    }
    if (next.temp < kTempMin) next.temp = kTempMin;
    if (next.temp > kTempMax) next.temp = kTempMax;
    if (setting_word && !power_word) next.on = true;
    state = next;
    return true;
}

uint8_t key_for(const State& before, const State& after) {
    if (before.on != after.on) return KEY_AC_POWER;
    if (before.mode != after.mode) return KEY_AC_MODE_SWITCH;
    if (after.temp > before.temp) return KEY_AC_TEMP_PLUS;
    if (after.temp < before.temp) return KEY_AC_TEMP_MINUS;
    if (before.fan != after.fan) return KEY_AC_WIND_SPEED;
    return KEY_AC_POWER;
}

void to_json(const State& s, cJSON* object) {
    for (const char* key : {"power", "mode", "temp", "fan"}) cJSON_DeleteItemFromObject(object, key);
    cJSON_AddStringToObject(object, "power", s.on ? "on" : "off");
    cJSON_AddNumberToObject(object, "mode", s.mode);
    cJSON_AddNumberToObject(object, "temp", s.temp);
    cJSON_AddNumberToObject(object, "fan", s.fan);
}

State from_json(const cJSON* object) {
    State s;
    if (!object) return s;
    s.on = json_str(object, "power", "on") != "off";
    const long mode = json_int(object, "mode", s.mode);
    const long temp = json_int(object, "temp", s.temp);
    const long fan = json_int(object, "fan", s.fan);
    if (mode >= 0 && mode < kModeCount) s.mode = static_cast<uint8_t>(mode);
    if (temp >= kTempMin && temp <= kTempMax) s.temp = static_cast<int>(temp);
    if (fan >= 0 && fan < kFanCount) s.fan = static_cast<uint8_t>(fan);
    return s;
}

bool parse_brand(const String& line, Brand& out) {
    const int a = line.indexOf('|');
    const int b = a < 0 ? -1 : line.indexOf('|', a + 1);
    if (a <= 0 || b < 0) return false;
    out = Brand();
    out.name = line.substring(0, a);
    out.name_en = line.substring(a + 1, b);
    String list = line.substring(b + 1);
    list.trim();
    std::size_t start = 0;
    while (start < list.length()) {
        int end = list.indexOf(',', start);
        if (end < 0) end = static_cast<int>(list.length());
        const String entry = list.substring(start, end);
        const int at = entry.indexOf('@'), plus = entry.indexOf('+');
        if (at <= 0 || plus < at) return false;
        Remote r;
        r.id = entry.substring(0, at);
        r.offset = static_cast<uint32_t>(std::strtoul(entry.substring(at + 1, plus).c_str(), nullptr, 10));
        r.length = static_cast<uint32_t>(std::strtoul(entry.substring(plus + 1).c_str(), nullptr, 10));
        if (r.length == 0) return false;
        out.remotes.push_back(r);
        start = end + 1;
    }
    return !out.remotes.empty();
}

bool brand_matches(const Brand& brand, const String& words) {
    if (words.isEmpty()) return false;
    if (words.indexOf(brand.name) >= 0) return true;
    if (brand.name.length() > words.length() && brand.name.indexOf(words) >= 0 && words.length() >= 6) return true;
    const auto squash = [](String s) {
        s.toLowerCase();
        String out;
        for (std::size_t i = 0; i < s.length(); ++i) {
            if (s[i] != ' ') out += s[i];
        }
        return out;
    };
    const String en = squash(brand.name_en);
    return !en.isEmpty() && squash(words) == en;
}

long index_length(const String& header_line) {
    if (!header_line.startsWith("IRAC1 ")) return -1;
    const String n = header_line.substring(6);
    for (std::size_t i = 0; i < n.length(); ++i) {
        if (n[i] < '0' || n[i] > '9') return -1;
    }
    return n.isEmpty() ? -1 : n.toInt();
}

bool encode(const std::vector<uint8_t>& code, State& state, uint8_t key, std::vector<uint16_t>& out, String& error) {
    out.clear();
    std::vector<uint8_t> binary(code);  // the decoder keeps a pointer while open
    if (binary.empty() || ir_binary_open(REMOTE_CATEGORY_AC, SUB_CATEGORY_QUATERNARY, binary.data(),
                                         static_cast<UINT16>(binary.size())) != IR_DECODE_SUCCEEDED) {
        error = "红外码文件打不开";
        return false;
    }
    bool ok = true;
    UINT8 modes = 0;
    if (state.on && (get_supported_mode(&modes) != IR_DECODE_SUCCEEDED || !(modes & (1u << state.mode)))) {
        String have;
        for (uint8_t m = 0; m < kModeCount; ++m) {
            if (modes & (1u << m)) have += String(have.isEmpty() ? "" : "、") + mode_label(m);
        }
        error = String("这套遥控码没有「") + mode_label(state.mode) + "」模式" + (have.isEmpty() ? "" : "，可用：" + have);
        ok = false;
    }
    if (ok && state.on) {
        INT8 low = -1, high = -1;
        if (get_temperature_range(state.mode, &low, &high) == IR_DECODE_SUCCEEDED && low >= 0 && high >= low) {
            if (state.temp < kTempMin + low) state.temp = kTempMin + low;
            if (state.temp > kTempMin + high) state.temp = kTempMin + high;
        }
        UINT8 fans = 0;
        if (get_supported_wind_speed(state.mode, &fans) == IR_DECODE_SUCCEEDED && fans && !(fans & (1u << state.fan))) {
            state.fan = kFanAuto;
        }
    }
    if (ok) {
        t_remote_ac_status status = {};
        status.ac_power = state.on ? AC_POWER_ON : AC_POWER_OFF;
        status.ac_mode = static_cast<t_ac_mode>(state.mode);
        status.ac_temp = static_cast<t_ac_temperature>(state.temp - kTempMin);
        status.ac_wind_speed = static_cast<t_ac_wind_speed>(state.fan);
        status.ac_wind_dir = AC_SWING_ON;
        out.resize(USER_DATA_SIZE);
        const UINT16 n = ir_decode(key, out.data(), &status);
        out.resize(n);
        if (n == 0) {
            error = "这套遥控码编不出这个状态";
            ok = false;
        }
    }
    ir_close();
    return ok;
}

}  // namespace vat::ac
