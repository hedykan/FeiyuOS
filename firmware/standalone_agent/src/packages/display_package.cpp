// display: full-screen overlays on the device's own screen and the one-line
// banner.  An overlay covers whatever page is open until any key (or the side
// button); nothing on the page underneath changes.  All targets are write:
//   device://local/display/text    content: text, shown as large as it fits
//   device://local/display/qr      content: the data to encode (URL, Wi-Fi string, ...)
//   device://local/display/image   content: path of a PNG / JPG / BMP under /vat/ (<= 32 KB)
//   device://local/display/banner  content: one short line on the Agent page
//   device://local/display/clear   closes the overlay
// Brightness, timeout and power stay with the local package.
#include "../common.h"
#include "../registry.h"
#include "../storage.h"
#include "../ui_state.h"

namespace vat::registry {
namespace {

constexpr char kPrefix[] = "device://local/display/";
constexpr std::size_t kMaxImage = 32768;  // PNG decoding needs ~40 KB more on top
constexpr std::size_t kMaxText = 600;

// Everything under the prefix except the local package's settings.  Models
// guess names (qrcode, notification, toast...), so common ones are accepted
// and anything else gets the list back instead of a dead end.
bool is_display_target(const String& target) {
    if (!target.startsWith(kPrefix)) return false;
    const String what = target.substring(strlen(kPrefix));
    return what != "brightness" && what != "timeout" && what != "power";
}

String canonical(String what) {
    what.toLowerCase();
    const int cut = what.indexOf('/');
    if (cut >= 0) what = what.substring(0, cut);
    static const char* const names[][2] = {
        {"text", "text"},     {"words", "text"},     {"bigtext", "text"},  {"big", "text"},    {"fullscreen", "text"},
        {"qr", "qr"},         {"qrcode", "qr"},      {"qr_code", "qr"},    {"qr-code", "qr"},
        {"image", "image"},   {"picture", "image"},  {"photo", "image"},   {"img", "image"},
        {"banner", "banner"}, {"notice", "banner"},  {"notification", "banner"}, {"notify", "banner"},
        {"toast", "banner"},  {"message", "banner"}, {"line", "banner"},   {"alert", "banner"},
        {"clear", "clear"},   {"close", "clear"},    {"off", "clear"},     {"dismiss", "clear"}, {"overlay", "clear"},
    };
    for (const auto& name : names) {
        if (what == name[0]) return name[1];
    }
    return "";
}

constexpr char kTargets[] =
    "可用 target：device://local/display/text（全屏文字）、/qr（二维码）、/image（/vat/ 下图片路径）、"
    "/banner（Agent 页一行通知）、/clear（关闭）";

String show_image(const String& path) {
    if (!path.startsWith("/vat/") || path.indexOf("..") >= 0) return "ERROR: 图片路径必须在 /vat/ 下";
    ui::Overlay overlay;
    overlay.kind = ui::Overlay::Kind::Image;
    overlay.image_path = path;
    if (!storage::read_bytes(path, overlay.image, kMaxImage)) {
        return storage::exists(path) ? "ERROR: 图片超过 32 KB，请缩小后再显示（JPG 最省内存）"
                                     : "ERROR: 找不到图片 " + path;
    }
    const auto& b = overlay.image;
    const bool known = (b.size() > 4 && b[0] == 0x89 && b[1] == 'P') || (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8) ||
                       (b.size() > 2 && b[0] == 'B' && b[1] == 'M');
    if (!known) return "ERROR: 只支持 PNG / JPG / BMP";
    ui::post_overlay(std::move(overlay));
    return "已全屏显示图片 " + path + "（任意键返回）";
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "在本机屏幕全屏显示用 write device://local/display/text、/qr 或 /image，Agent 页一行通知用 device://local/display/banner。";

class DisplayPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    bool reversible(const String&, const cJSON*) const override { return true; }

    const Manifest& manifest() const override {
        static const Manifest m{"display", "在本机屏幕上全屏显示文字、二维码、图片，或在 Agent 页显示一行通知",
                                "屏幕 显示 展示 全屏 大字 文字 二维码 QR 图片 照片 通知 横幅 提示 给别人看 投屏 关闭显示", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "write" && is_display_target(json_str(arguments, "target"));
    }

    String execute(const String&, const cJSON* arguments, bool) override {
        const String what = canonical(json_str(arguments, "target").substring(strlen(kPrefix)));
        if (what.isEmpty()) return String("ERROR: 没有这个显示目标。") + kTargets;
        String content = json_str(arguments, "content");
        content.trim();
        if (what == "clear") {
            ui::post_overlay(ui::Overlay());
            return "已关闭全屏显示";
        }
        if (content.isEmpty()) return "ERROR: 缺少要显示的内容";
        if (what == "banner") {
            ui::set_notice(utf8_head(content, 120));
            return "已在 Agent 页显示通知";
        }
        if (what == "image") return show_image(content);
        ui::Overlay overlay;
        overlay.kind = what == "qr" ? ui::Overlay::Kind::Qr : ui::Overlay::Kind::Text;
        overlay.text = utf8_head(content, what == "qr" ? 512 : kMaxText);
        ui::post_overlay(std::move(overlay));
        return what == "qr" ? "已全屏显示二维码（任意键返回）" : "已全屏显示文字（任意键返回）";
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"显示", "", "把 WiFi 名称大字显示在屏幕上"});
        out.push_back({"二维码", "", "把这个网址显示成二维码"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"device://local/display/text", "display：全屏大字，content 为文字"},
            {"device://local/display/qr", "display：全屏二维码，content 为要编码的内容"},
            {"device://local/display/image", "display：全屏图片，content 为 /vat/ 下 PNG/JPG/BMP 路径（≤32KB）"},
            {"device://local/display/banner", "display：Agent 页一行通知，content 为文字"},
            {"device://local/display/clear", "display：关闭全屏显示，content 为 clear"},
        };
        for (const auto& item : items) {
            add_resource(list, remaining, item[0], item[1]);
        }
    }
};

}  // namespace

Package* make_display_package() { return new DisplayPackage(); }

}  // namespace vat::registry
