#include "gfx_api.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ui_canvas.h"

namespace vat::gfx_api {
namespace {

using G = Group;

// M5GFX's LGFXBase calls (signatures from LGFXBase.hpp), and the queries a
// program needs answered at once.  Not here: what needs pixels already on the
// screen (readPixel, floodFill, copyRect, scroll), sprites, and the hardware
// calls (startWrite, writePixels, DMA, setRotation, loadFont): see G2b.
const Fn kTable[] = {
    // ---- shapes; the colour may be left out: then setColor's
    {"fillScreen", "[c", "'#101820'", G::Draw},
    {"clear", "[c", "", G::Draw},
    {"drawPixel", "ii[c", "5, 5, 'white'", G::Draw},
    {"drawFastVLine", "iii[c", "8, 20, 30, 'white'", G::Draw},
    {"drawFastHLine", "iii[c", "8, 20, 30, 'white'", G::Draw},
    {"drawRect", "iiii[c", "10, 10, 40, 20, 'cyan'", G::Draw},
    {"fillRect", "iiii[c", "10, 40, 40, 20, 'red'", G::Draw},
    {"drawRoundRect", "iiiii[c", "60, 10, 40, 20, 5, 'cyan'", G::Draw},
    {"fillRoundRect", "iiiii[c", "60, 40, 40, 20, 5, 'orange'", G::Draw},
    {"drawCircle", "iii[c", "130, 30, 12, 'white'", G::Draw},
    {"fillCircle", "iii[c", "130, 60, 10, 'green'", G::Draw},
    {"drawEllipse", "iiii[c", "170, 30, 20, 10, 'white'", G::Draw},
    {"fillEllipse", "iiii[c", "170, 60, 20, 10, 'purple'", G::Draw},
    {"drawLine", "iiii[c", "0, 0, 239, 134, 'gray'", G::Draw},
    {"drawTriangle", "iiiiii[c", "10, 120, 30, 90, 50, 120, 'white'", G::Draw},
    {"fillTriangle", "iiiiii[c", "60, 120, 80, 90, 100, 120, 'yellow'", G::Draw},
    {"drawBezier", "iiiiii[c|iiiiiiiic", "110, 120, 130, 80, 150, 120, 'cyan'", G::Draw},
    {"drawEllipseArc", "iiiiiiff[c", "200, 100, 10, 14, 8, 12, 0, 270, 'white'", G::Draw},
    {"fillEllipseArc", "iiiiiiff[c", "200, 100, 4, 8, 3, 6, 0, 180, 'red'", G::Draw},
    {"drawArc", "iiiiff[c", "40, 100, 10, 14, 0, 270, 'white'", G::Draw},
    {"fillArc", "iiiiff[c", "40, 100, 4, 8, 90, 360, 'cyan'", G::Draw},
    {"drawGradientHLine", "iiicc", "10, 5, 100, 'red', 'blue'", G::Draw},
    {"drawGradientVLine", "iiicc", "5, 10, 100, 'red', 'blue'", G::Draw},
    {"drawGradientLine", "iiiicc", "0, 134, 239, 0, 'green', 'purple'", G::Draw},
    {"fillGradientRect", "iiiicc[S", "150, 80, 60, 30, 'red', 'blue', 'horizontal_linear'", G::Draw},
    {"drawSmoothLine", "iiiic", "0, 130, 239, 100, 'white'", G::Draw},
    {"drawWideLine", "iiiifc", "20, 20, 120, 60, 3, 'orange'", G::Draw},
    {"drawWedgeLine", "iiiiffc", "20, 60, 120, 20, 1, 5, 'cyan'", G::Draw},
    {"drawSpot", "iifc", "220, 20, 6.5, 'yellow'", G::Draw},
    {"fillSmoothCircle", "iii[c", "220, 50, 9, 'green'", G::Draw},
    {"fillSmoothRoundRect", "iiiii[c", "180, 110, 50, 20, 6, 'gray'", G::Draw},
    {"fillRectAlpha", "iiiiic", "0, 0, 120, 67, 128, 'blue'", G::Draw},
    {"drawCircleHelper", "iiii[c", "100, 100, 12, 3, 'white'", G::Draw},
    {"fillCircleHelper", "iiiii[c", "100, 100, 8, 3, 4, 'white'", G::Draw},
    {"fillAffine", "Mii[c", "{1, 0, 100, 0, 1, 50}, 20, 10, 'pink'", G::Draw},
    // ---- images and bitmaps (bytes and files count against kMaxBytes a frame)
    {"drawBitmap", "iiBiic[c", "200, 5, {24, 60, 126, 255, 255, 126, 60, 24}, 8, 8, 'white'", G::Draw},
    {"drawXBitmap", "iiBiic[c", "210, 5, {24, 60, 126, 255, 255, 126, 60, 24}, 8, 8, 'red', 'black'", G::Draw},
    {"pushImage", "iiiiD[c", "220, 5, 2, 2, {63488, 2016, 31, 65535}", G::Draw},
    {"pushImageRotateZoom", "fffffffiiD[c", "230, 10, 1, 1, 45, 2, 2, 2, 2, {63488, 2016, 31, 65535}", G::Draw},
    {"drawPng", "P[iiiiiiffd", "", G::Draw},
    {"drawJpg", "P[iiiiiiffd", "", G::Draw},
    {"drawBmp", "P[iiiiiiffd", "", G::Draw},
    {"qrcode", "s[iiii", "'FeiyuOS', 170, 70, 60", G::Draw},
    // ---- text
    {"drawString", "sii[F", "'你好 gfx', 10, 70", G::Draw},
    {"drawCentreString", "sii[F", "'居中', 120, 90", G::Draw},
    {"drawCenterString", "sii[F", "'center', 120, 100", G::Draw},
    {"drawRightString", "sii[F", "'右', 236, 110", G::Draw},
    {"drawNumber", "iii[F", "42, 10, 85", G::Draw},
    {"drawFloat", "fiii[F", "3.14159, 2, 40, 85", G::Draw},
    {"drawChar", "uii[F|iiuccf[f", "'A', 80, 85", G::Draw},
    {"print", "s", "'hello '", G::Draw},
    {"println", "[s", "'world'", G::Draw},
    // ---- state: kept for the rest of the frame, and the next frames
    {"setCursor", "ii[F", "4, 4", G::State},
    {"setTextColor", "c[c", "'white'", G::State},
    {"setTextSize", "f[f", "1", G::State},
    {"setTextDatum", "d", "'top_left'", G::State},
    {"setTextWrap", "b[b", "false", G::State},
    {"setTextPadding", "i", "0", G::State},
    {"setTextScroll", "b", "false", G::State},
    {"setFont", "F", "'cn12'", G::State},
    {"setTextFont", "F", "'cn12'", G::State},
    {"setColor", "c|iii", "'white'", G::State},
    {"setBaseColor", "c", "'black'", G::State},
    {"setClipRect", "iiii", "0, 0, 240, 135", G::State},
    {"clearClipRect", "", "", G::State},
    // ---- queries (programs: answered at once)
    {"width", "", "", G::Query},
    {"height", "", "", G::Query},
    {"textWidth", "s[F", "'abc'", G::Query},
    {"textLength", "si", "'abcdef', 20", G::Query},
    {"fontHeight", "[F", "", G::Query},
    {"fontWidth", "[F", "", G::Query},
    {"getCursorX", "", "", G::Query},
    {"getCursorY", "", "", G::Query},
    {"color565", "iii", "255, 0, 0", G::Query},
    {"color888", "iii", "255, 0, 0", G::Query},
    {"color332", "iii", "255, 0, 0", G::Query},
};

const char* const kFonts[] = {"cn12", "cn14", "Font0", "Font2", "Font4", "Font6", "Font7", "Font8"};

struct Datum {
    const char* name;
    int value;
};
const Datum kDatums[] = {{"top_left", 0},       {"top_center", 1},    {"top_centre", 1},      {"top_right", 2},
                         {"middle_left", 4},    {"middle_center", 5}, {"middle_centre", 5},   {"middle_right", 6},
                         {"bottom_left", 8},    {"bottom_center", 9}, {"bottom_centre", 9},   {"bottom_right", 10},
                         {"baseline_left", 16}, {"baseline_center", 17}, {"baseline_centre", 17}, {"baseline_right", 18}};

bool placeholder(const cJSON* item) {
    if (!cJSON_IsString(item)) return false;
    const std::size_t n = std::strlen(item->valuestring);
    return n >= 3 && item->valuestring[0] == '{' && item->valuestring[n - 1] == '}';
}

String hex_byte(unsigned v) {
    static const char* const digits = "0123456789ABCDEF";
    String out;
    out += digits[(v >> 4) & 0xF];
    out += digits[v & 0xF];
    return out;
}

bool is_hex(const char* s, std::size_t multiple) {
    const std::size_t n = std::strlen(s);
    if (n == 0 || n % multiple) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

String number_text(double v) {
    if (std::fabs(v - std::round(v)) < 1e-9) return String(static_cast<long>(std::lround(v)));
    return String(v, 4);
}

// One argument of type `type`; false with `error` set.
bool one(char type, const cJSON* item, String& out, std::size_t& bytes, String& error) {
    switch (type) {
        case 'i':
        case 'f':
            if (placeholder(item)) return out = item->valuestring, true;
            if (!cJSON_IsNumber(item)) return error = "要是数字", false;
            if (item->valuedouble < kMinCoord || item->valuedouble > kMaxCoord) return error = "数值超出范围", false;
            out = type == 'i' ? String(static_cast<long>(std::lround(item->valuedouble))) : number_text(item->valuedouble);
            return true;
        case 'c': {
            if (placeholder(item)) return out = item->valuestring, true;
            uint32_t rgb = 0;
            if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 0xFFFF) {  // RGB565, as M5GFX
                const unsigned v = static_cast<unsigned>(item->valuedouble);
                const unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
                rgb = ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
            } else if (!cJSON_IsString(item) || !ui_canvas::color(item->valuestring, rgb)) {
                return error = "颜色要写 #RRGGBB、white、red 这类名字，或 RGB565 数字（color565(r, g, b)）", false;
            }
            out = "#" + hex_byte(rgb >> 16) + hex_byte(rgb >> 8) + hex_byte(rgb);
            return true;
        }
        case 's':
            if (cJSON_IsNumber(item)) return out = number_text(item->valuedouble), true;
            if (!cJSON_IsString(item)) return error = "要是文字", false;
            out = utf8_head(String(item->valuestring), kMaxText);
            return true;
        case 'F': {
            String name = cJSON_IsNumber(item) ? "Font" + String(static_cast<int>(item->valuedouble))
                                               : cJSON_IsString(item) ? String(item->valuestring) : String();
            for (const char* f : kFonts) {
                if (name.length() != std::strlen(f)) continue;
                bool same = true;
                for (std::size_t i = 0; same && i < name.length(); ++i) same = (name[i] | 0x20) == (f[i] | 0x20);
                if (same) return out = f, true;
            }
            return error = "字体只能是 cn12、cn14（中文）或 Font0、Font2、Font4、Font6、Font7、Font8", false;
        }
        case 'b':
            if (cJSON_IsBool(item)) return out = cJSON_IsTrue(item) ? "1" : "0", true;
            if (cJSON_IsNumber(item)) return out = item->valuedouble != 0 ? "1" : "0", true;
            return error = "要是 true 或 false", false;
        case 'd':
            if (cJSON_IsNumber(item)) {
                for (const auto& d : kDatums) {
                    if (d.value == item->valuedouble) return out = String(d.value), true;
                }
            } else if (cJSON_IsString(item) && datum_of(item->valuestring) >= 0) {
                return out = String(datum_of(item->valuestring)), true;
            }
            return error = "对齐方式要写 top_left、middle_center、bottom_right 这类", false;
        case 'S': {
            const String v = cJSON_IsString(item) ? String(item->valuestring) : String();
            if (v == "horizontal_linear" || v == "HLINEAR" || (cJSON_IsNumber(item) && item->valuedouble == 0)) return out = "0", true;
            if (v == "vertical_linear" || v == "VLINEAR" || (cJSON_IsNumber(item) && item->valuedouble == 1)) return out = "1", true;
            if (v == "radial_center" || v == "RADIAL" || (cJSON_IsNumber(item) && item->valuedouble == 2)) return out = "2", true;
            return error = "渐变方式要写 horizontal_linear、vertical_linear 或 radial_center", false;
        }
        case 'M': {
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 6) return error = "要是 6 个数字的数组", false;
            out = "";
            for (int k = 0; k < 6; ++k) {
                const cJSON* v = cJSON_GetArrayItem(item, k);
                if (!cJSON_IsNumber(v)) return error = "矩阵要是 6 个数字", false;
                out += (k ? "," : "") + number_text(v->valuedouble);
            }
            return true;
        }
        case 'u':
            if (cJSON_IsNumber(item)) return out = String(static_cast<long>(item->valuedouble)), true;
            if (cJSON_IsString(item) && *item->valuestring) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(item->valuestring);
                uint32_t cp = p[0];
                if (p[0] >= 0xF0) cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                else if (p[0] >= 0xE0) cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                else if (p[0] >= 0xC0) cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                return out = String(cp), true;
            }
            return error = "要是一个字或它的编码", false;
        case 'B':
        case 'D': {
            const unsigned top = type == 'B' ? 0xFF : 0xFFFF;
            const std::size_t width = type == 'B' ? 1 : 2;
            out = "";
            if (cJSON_IsString(item) && is_hex(item->valuestring, width * 2)) {
                out = item->valuestring;
            } else if (cJSON_IsArray(item) && cJSON_GetArraySize(item) > 0) {
                const cJSON* v = nullptr;
                cJSON_ArrayForEach(v, item) {
                    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > top) {
                        return error = type == 'B' ? "位图要是 0 到 255 的字节数组" : "像素要是 RGB565 数字（0 到 65535）的数组", false;
                    }
                    const unsigned n = static_cast<unsigned>(v->valuedouble);
                    if (width == 2) out += hex_byte(n >> 8);
                    out += hex_byte(n);
                }
            } else {
                return error = type == 'B' ? "位图要是字节数组" : "像素要是 RGB565 数字的数组", false;
            }
            bytes += out.length() / 2;
            if (bytes > kMaxBytes) return error = "一帧的位图、像素和图片合计不能超过 32 KB", false;
            return true;
        }
        case 'P':
            if (!cJSON_IsString(item) || std::strncmp(item->valuestring, "/vat/", 5)) return error = "只能画 /vat/ 下的图片", false;
            out = item->valuestring;
            return true;
        default:
            return error = "参数类型未知", false;
    }
}

}  // namespace

const Fn* table() { return kTable; }
std::size_t count() { return sizeof(kTable) / sizeof(kTable[0]); }

const Fn* find(const String& name) {
    for (const auto& fn : kTable) {
        if (name == fn.name) return &fn;
    }
    return nullptr;
}

int index_of(const Fn* fn) { return fn ? static_cast<int>(fn - kTable) : -1; }

String overload_params(const Fn& fn, int overload) {
    String all(fn.params);
    for (int k = 0; k < overload; ++k) {
        const int bar = all.indexOf('|');
        if (bar < 0) return String();
        all = all.substring(bar + 1);
    }
    const int bar = all.indexOf('|');
    if (bar >= 0) all = all.substring(0, bar);
    String types;
    for (std::size_t i = 0; i < all.length(); ++i) {
        if (all[i] != '[') types += all[i];
    }
    return types;
}

bool check(const Fn& fn, const cJSON* args, std::vector<String>& out, int& overload, std::size_t& bytes,
           String& error) {
    const int n = cJSON_IsArray(args) ? cJSON_GetArraySize(args) : 0;
    String all(fn.params);
    String first_error;
    for (int k = 0;; ++k) {
        const int bar = all.indexOf('|');
        const String spec = bar < 0 ? all : all.substring(0, bar);
        const int optional = spec.indexOf('[');
        const String types = optional < 0 ? spec : spec.substring(0, optional) + spec.substring(optional + 1);
        const int required = optional < 0 ? spec.length() : optional;
        if (n >= required && n <= static_cast<int>(types.length())) {
            std::vector<String> values;
            std::size_t used = bytes;
            String why;
            bool ok = true;
            for (int i = 0; ok && i < n; ++i) {
                String v;
                ok = one(types[i], cJSON_GetArrayItem(args, i), v, used, why);
                if (ok) values.push_back(v);
                else why = "第 " + String(i + 1) + " 个参数" + why;
            }
            if (ok) {
                out = std::move(values);
                overload = k;
                bytes = used;
                return true;
            }
            if (first_error.isEmpty()) first_error = why;
        } else if (first_error.isEmpty()) {
            first_error = "参数个数不对（" + String(n) + " 个）";
        }
        if (bar < 0) break;
        all = all.substring(bar + 1);
    }
    error = String(fn.name) + "：" + first_error + "；写法见 read gfx://help";
    return false;
}

const char* const* font_names(std::size_t& n) {
    n = sizeof(kFonts) / sizeof(kFonts[0]);
    return kFonts;
}

int datum_of(const String& name) {
    for (const auto& d : kDatums) {
        if (name == d.name) return d.value;
    }
    return -1;
}

uint16_t rgb565(uint32_t rgb) {
    return static_cast<uint16_t>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

String help() {
    static const char* const kNames[] = {"i 整数", "f 数", "c 颜色", "s 文字", "F 字体", "b 真假", "d 对齐",
                                         "S 渐变方式", "M 6 个数", "u 一个字", "B 字节数组", "D RGB565 数组",
                                         "P /vat/ 下图片"};
    String out = "gfx 函数（与 M5GFX 同名同参；程序里 -- @uses gfx 后写 gfx.名字(参数)，画完 gfx.present()；"
                 "gfx://frame 和画布的 ops 写成 [名字, 参数…]）。参数类型：";
    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) out += (i ? "、" : "") + String(kNames[i]);
    out += "；[ 之后的可省略，| 分隔几种写法；颜色省略时用 setColor 的颜色。\n";
    static const char* const kGroups[] = {"画", "状态（保持到改掉为止，下一帧继续有效）", "查询（只在程序里，立即返回）"};
    for (int g = 0; g < 3; ++g) {
        out += String("\n") + kGroups[g] + "：\n";
        for (const auto& fn : kTable) {
            if (static_cast<int>(fn.group) != g) continue;
            out += String(fn.name) + "(" + fn.params + ")";
            if (*fn.example) out += String(" 例 gfx.") + fn.name + "(" + fn.example + ")";
            out += "\n";
        }
    }
    out += "\nFeiyuOS 自己的：progressBar(x y 宽 高 百分比 颜色 [底色])、rotate(角度 中心x 中心y，只转形状)、"
           "drawQr(文字 x y 宽)、画布里的 drawImage(/vat/图片 x y)；程序里另有 present()、size()、measure(文字, size)、close()，"
           "颜色常量 gfx.TFT_RED 这类。字体：cn12、cn14（中文），Font0、Font2、Font4、Font6、Font7、Font8（英文数字）。"
           "不提供：readPixel、floodFill、copyRect、scroll、sprite（0.8 独占态），以及 startWrite、DMA 这类底层调用。";
    return out;
}

}  // namespace vat::gfx_api
