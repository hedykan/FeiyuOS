#include "ui_canvas.h"

#include <cJSON.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ui_panel.h"

namespace vat::ui_canvas {
namespace {

struct Shape {
    const char* name;
    Kind kind;
    int numbers;      // numeric arguments before the colour(s)
    int colors;       // 1, or 2 for gradient / progress background (optional second)
};

// Argument shapes follow the M5GFX (LovyanGFX) calls of the same name.
const Shape kShapes[] = {
    {"fillScreen", Kind::FillScreen, 0, 1},
    {"drawPixel", Kind::DrawPixel, 2, 1},
    {"drawLine", Kind::DrawLine, 4, 1},
    {"drawFastHLine", Kind::DrawFastHLine, 3, 1},
    {"drawFastVLine", Kind::DrawFastVLine, 3, 1},
    {"drawRect", Kind::DrawRect, 4, 1},
    {"fillRect", Kind::FillRect, 4, 1},
    {"drawRoundRect", Kind::DrawRoundRect, 5, 1},
    {"fillRoundRect", Kind::FillRoundRect, 5, 1},
    {"drawCircle", Kind::DrawCircle, 3, 1},
    {"fillCircle", Kind::FillCircle, 3, 1},
    {"drawEllipse", Kind::DrawEllipse, 4, 1},
    {"fillEllipse", Kind::FillEllipse, 4, 1},
    {"drawTriangle", Kind::DrawTriangle, 6, 1},
    {"fillTriangle", Kind::FillTriangle, 6, 1},
    {"drawArc", Kind::DrawArc, 6, 1},      // x y r0 r1 angle0 angle1
    {"fillArc", Kind::FillArc, 6, 1},
    {"drawBezier", Kind::DrawBezier, 6, 1},  // quadratic: x0 y0 x1 y1 x2 y2
    {"drawGradientLine", Kind::DrawGradientLine, 4, 2},
    {"drawString", Kind::DrawString, 0, 0},  // text x y {options}
    {"drawQr", Kind::DrawQr, 0, 0},          // text x y size
    {"drawImage", Kind::DrawImage, 0, 0},    // path x y
    {"progressBar", Kind::ProgressBar, 5, 2},  // x y w h value(0-100) colour [background]
    {"rotate", Kind::Rotate, 3, 0},            // angle cx cy (0 0 0 stops)
};

struct Named {
    const char* name;
    uint32_t rgb;
};
const Named kColors[] = {
    {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xE5534B},    {"green", 0x3FB950}, {"blue", 0x388BFD},
    {"yellow", 0xE3B341}, {"orange", 0xE8A33D}, {"cyan", 0x3FB8AF}, {"purple", 0xA371F7}, {"pink", 0xF778BA},
    {"gray", 0x7A8494},  {"grey", 0x7A8494},   {"bg", 0x0F1216},     {"accent", 0x3FB8AF},
};
const char* const kDatums[] = {"top_left", "top_center", "top_right", "middle_left", "middle_center",
                               "middle_right", "bottom_left", "bottom_center", "bottom_right"};

bool placeholder(const String& text) {  // {1}, {2#} …
    return text.length() >= 3 && text[0] == '{' && text[text.length() - 1] == '}';
}

// A numeric argument: a number in range, or a "{n}" filled in at refresh.
bool numeric_arg(const cJSON* item, String& out, String& error, const String& where) {
    if (cJSON_IsNumber(item)) {
        const double v = item->valuedouble;
        if (v < kMinCoord * 4 || v > kMaxCoord * 4) {
            error = where + " 的数值超出范围";
            return false;
        }
        out = String(static_cast<long>(v));
        return true;
    }
    if (cJSON_IsString(item) && placeholder(String(item->valuestring))) {
        out = item->valuestring;
        return true;
    }
    error = where + " 要是数字（或 {1} 这类数据位）";
    return false;
}

bool text_arg(const cJSON* item, String& out, String& error, const String& where) {
    if (!cJSON_IsString(item)) {
        error = where + " 要是文字";
        return false;
    }
    out = utf8_head(String(item->valuestring), kMaxText);
    return true;
}

bool color_arg(const cJSON* item, String& out, String& error, const String& where) {
    uint32_t rgb;
    if (!cJSON_IsString(item) || (!placeholder(String(item->valuestring)) && !color(String(item->valuestring), rgb))) {
        error = where + " 的颜色要写 #RRGGBB 或 white、red、cyan 这类名字";
        return false;
    }
    out = item->valuestring;
    return true;
}

bool parse_op(const cJSON* item, int index, Op& op, Spec& spec, String& error) {
    const String where = "第 " + String(index + 1) + " 条";
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) < 1 || !cJSON_IsString(cJSON_GetArrayItem(item, 0))) {
        error = where + "要写成 [指令名, 参数…]";
        return false;
    }
    const String name = cJSON_GetArrayItem(item, 0)->valuestring;
    const Shape* shape = nullptr;
    for (const auto& s : kShapes) {
        if (name == s.name) shape = &s;
    }
    if (!shape) {
        error = where + "：不支持的指令 " + name;
        return false;
    }
    op.kind = shape->kind;
    const int n = cJSON_GetArraySize(item) - 1;
    const auto arg = [item](int i) { return cJSON_GetArrayItem(item, i + 1); };
    const String at = where + "（" + name + "）";
    if (op.kind == Kind::DrawString || op.kind == Kind::DrawQr || op.kind == Kind::DrawImage) {
        const int needed = op.kind == Kind::DrawQr ? 4 : 3;
        if (n < needed) {
            error = at + "参数不够";
            return false;
        }
        String text, x, y;
        if (!text_arg(arg(0), text, error, at) || !numeric_arg(arg(1), x, error, at) || !numeric_arg(arg(2), y, error, at)) {
            return false;
        }
        op.args = {text, x, y};
        if (op.kind == Kind::DrawQr) {
            String size;
            if (!numeric_arg(arg(3), size, error, at)) return false;
            op.args.push_back(size);
        }
        if (op.kind == Kind::DrawImage) {
            if (!text.startsWith("/vat/")) {
                error = at + "只能画 /vat/ 下的图片";
                return false;
            }
            if (++spec.images > 1) {
                error = "一张画布最多一张图片";
                return false;
            }
        }
        if (op.kind == Kind::DrawString && n >= 4) {
            const cJSON* options = arg(3);
            if (!cJSON_IsObject(options)) {
                error = at + "第 4 个参数要是 {size, color, datum}";
                return false;
            }
            op.size = static_cast<int>(json_int(options, "size", 1));
            if (op.size < 1 || op.size > 4) {
                error = at + "size 只能 1 到 4";
                return false;
            }
            const cJSON* c = cJSON_GetObjectItemCaseSensitive(options, "color");
            if (c && !color_arg(c, op.color, error, at)) return false;
            const String datum = json_str(options, "datum", "top_left");
            bool known = false;
            for (const char* d : kDatums) known = known || datum == d;
            if (!known) {
                error = at + "datum 只能是 top_left、middle_center、bottom_right 这类";
                return false;
            }
            op.datum = datum;
        }
        return true;
    }
    const int required = shape->numbers + (shape->colors ? 1 : 0);
    if (n < required || n > shape->numbers + shape->colors) {
        error = at + "要 " + String(shape->numbers) + " 个数字加颜色";
        return false;
    }
    for (int i = 0; i < shape->numbers; ++i) {
        String value;
        if (!numeric_arg(arg(i), value, error, at)) return false;
        op.args.push_back(value);
    }
    if (shape->colors == 0) return true;
    if (!color_arg(arg(shape->numbers), op.color, error, at)) return false;
    if (n > shape->numbers + 1 && !color_arg(arg(shape->numbers + 1), op.color2, error, at)) return false;
    if (op.kind == Kind::DrawGradientLine && op.color2.isEmpty()) {
        error = at + "要两个颜色（起点、终点）";
        return false;
    }
    return true;
}

// Placeholders written without quotes ([fillCircle, 120, {t}, 9]) are not
// JSON; quote them so the description parses.  Only {t}, {1} and {1#} outside
// strings: a JSON object always starts with {".
String quote_placeholders(const String& json) {
    String out;
    out.reserve(json.length() + 16);
    bool in_string = false;
    for (unsigned i = 0; i < json.length(); ++i) {
        const char c = json[i];
        if (in_string) {
            out += c;
            if (c == '\\' && i + 1 < json.length()) out += json[++i];
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        if (c == '{') {
            unsigned j = i + 1;
            if (j < json.length() && json[j] == 't') {
                ++j;
            } else {
                while (j < json.length() && isdigit(static_cast<unsigned char>(json[j]))) ++j;
                if (j > i + 1 && j < json.length() && json[j] == '#') ++j;
            }
            if (j > i + 1 && j < json.length() && json[j] == '}') {
                out += '"';
                out += json.substring(i, j + 1);
                out += '"';
                i = j;
                continue;
            }
        }
        out += c;
    }
    return out;
}

}  // namespace

bool parse(const String& json, Spec& out, String& error) {
    out = Spec();
    if (json.length() > kMaxJson) {
        error = "画布描述超过 " + String(static_cast<int>(kMaxJson)) + " 字节";
        return false;
    }
    const String text = quote_placeholders(json);
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithOpts(text.c_str(), &end, false);
    if (!root) {  // say where, so the next attempt can fix it
        const long at = end ? static_cast<long>(end - text.c_str()) : 0;
        const long from = at > 40 ? at - 40 : 0;
        error = "画布 JSON 第 " + String(at) + " 字节附近格式错误：…" +
                utf8_sanitize(text.substring(from, at)) + "⟨这里⟩" + utf8_sanitize(text.substring(at, at + 20)) +
                "…（键值用冒号，字符串和占位符加双引号）";
        return false;
    }
    const cJSON* ops = cJSON_GetObjectItemCaseSensitive(root, "ops");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(ops)) {
        cJSON_Delete(root);
        error = "画布描述要是 JSON 对象，含 ops 数组";
        return false;
    }
    const cJSON* field = nullptr;
    cJSON_ArrayForEach(field, root) {
        if (std::strcmp(field->string, "ops") && std::strcmp(field->string, "source") && std::strcmp(field->string, "animate")) {
            error = String("不认识的字段 ") + field->string + "（只能有 ops、source、animate）";
            cJSON_Delete(root);
            return false;
        }
    }
    const int count = cJSON_GetArraySize(ops);
    bool ok = count > 0;
    if (count == 0) error = "ops 是空的";
    if (static_cast<std::size_t>(count) > kMaxOps) {
        error = "ops 最多 " + String(static_cast<int>(kMaxOps)) + " 条";
        ok = false;
    }
    for (int i = 0; ok && i < count; ++i) {
        Op op;
        ok = parse_op(cJSON_GetArrayItem(ops, i), i, op, out, error);
        if (ok) out.ops.push_back(op);
    }
    const cJSON* animate = cJSON_GetObjectItemCaseSensitive(root, "animate");
    if (ok && animate) {
        const String every = json_str(animate, "every", "100ms");
        long ms = every.toInt();
        if (every.endsWith("ms")) ms = every.substring(0, every.length() - 2).toInt();
        else if (every.endsWith("s")) ms = every.substring(0, every.length() - 1).toInt() * 1000;
        const long frames = json_int(animate, "frames", 0);
        const long step = json_int(animate, "step", 1);
        if (!cJSON_IsObject(animate) || ms < static_cast<long>(kMinFrameMs) || ms > 10000) {
            error = "animate.every 要在 80ms 到 10s 之间，如 100ms";
            ok = false;
        } else if (frames < 0 || frames > static_cast<long>(kMaxFrames) || step < -360 || step > 360) {
            error = "animate.frames 最多 600（0 表示一直到关闭），step 在 -360 到 360";
            ok = false;
        } else {
            out.frame_ms = static_cast<uint32_t>(ms);
            out.frames = static_cast<uint32_t>(frames);
            out.step = static_cast<int>(step);
        }
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(root, "source")) {
        // The same source block as a panel's.
        cJSON* wrapper = cJSON_CreateObject();
        cJSON_AddItemToObject(wrapper, "source", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root, "source"), true));
        ui_panel::Spec panel;
        ok = ui_panel::parse(json_print(wrapper), panel, error);
        cJSON_Delete(wrapper);
        out.source_tool = panel.source_tool;
        out.source_args = panel.source_args;
        out.every_s = panel.every_s;
    }
    cJSON_Delete(root);
    return ok;
}

bool number(const String& text, int& out);

void fill(std::vector<Op>& ops, const std::vector<String>& lines) {
    const auto replace = [&lines](String& text) {
        for (int n = 1; n <= 12 && text.indexOf('{') >= 0; ++n) {
            const String line = n <= static_cast<int>(lines.size()) ? lines[n - 1] : String();
            // {n#}: the first number in line n ("电量 87%，…" gives 87).
            int first = 0;
            const String digits = number(line, first) ? String(first) : String();
            const String keys[2] = {"{" + String(n) + "#}", "{" + String(n) + "}"};
            const String values[2] = {digits, line};
            for (int k = 0; k < 2; ++k) {
                int at;
                while ((at = text.indexOf(keys[k])) >= 0) {
                    text = text.substring(0, at) + values[k] + text.substring(at + keys[k].length());
                }
            }
        }
    };
    for (auto& op : ops) {
        for (auto& a : op.args) replace(a);
        replace(op.color);
        replace(op.color2);
    }
}

void fill_frame(std::vector<Op>& ops, long t) {
    const String value(t);
    for (auto& op : ops) {
        for (auto& a : op.args) {
            int at;
            while ((at = a.indexOf("{t}")) >= 0) a = a.substring(0, at) + value + a.substring(at + 3);
        }
    }
}

void rotate_point(int degrees, int cx, int cy, int& x, int& y) {
    // Integer sine table in 1/1024 steps every degree would cost 1.4 KB of
    // flash; the float functions are fine at canvas sizes.
    const float r = static_cast<float>(degrees % 360) * 3.14159265f / 180.0f;
    const float c = std::cos(r), s = std::sin(r);
    const float dx = static_cast<float>(x - cx), dy = static_cast<float>(y - cy);
    x = cx + static_cast<int>(std::lround(dx * c - dy * s));
    y = cy + static_cast<int>(std::lround(dx * s + dy * c));
}

bool color(const String& text, uint32_t& rgb) {
    String t = text;
    t.trim();
    if (t.startsWith("#")) {
        const String hex = t.substring(1);
        if (hex.length() != 3 && hex.length() != 6) return false;
        for (std::size_t i = 0; i < hex.length(); ++i) {
            const char c = hex[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
        const unsigned long v = std::strtoul(hex.c_str(), nullptr, 16);
        rgb = hex.length() == 6 ? static_cast<uint32_t>(v)
                                : static_cast<uint32_t>(((v >> 8) & 0xF) * 0x110000 + ((v >> 4) & 0xF) * 0x1100 + (v & 0xF) * 0x11);
        return true;
    }
    t.toLowerCase();
    for (const auto& c : kColors) {
        if (t == c.name) {
            rgb = c.rgb;
            return true;
        }
    }
    return false;
}

bool number(const String& text, int& out) {
    String t = text;
    t.trim();
    // A source line like "电量 87%" yields its first number.
    std::size_t i = 0;
    while (i < t.length() && !(t[i] == '-' || (t[i] >= '0' && t[i] <= '9'))) ++i;
    if (i >= t.length()) return false;
    char* end = nullptr;
    const long v = std::strtol(t.c_str() + i, &end, 10);
    if (end == t.c_str() + i) return false;
    out = static_cast<int>(v);
    return true;
}

const char* kind_name(Kind kind) {
    for (const auto& s : kShapes) {
        if (s.kind == kind) return s.name;
    }
    return "?";
}

}  // namespace vat::ui_canvas
