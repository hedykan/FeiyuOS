// ble: Bluetooth LE, all four roles (src/ops_ble.h); addresses per docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §5.  Off unless turned on in
// the settings (takes effect after a restart).
//   read    device://local/ble/status | device://local/ble/scan[?seconds=5] | device://local/ble/devices/list | device://local/ble/device/ADDR[/services]
//   read    device://local/ble/gatt/ADDR/SERVICE/CHAR            read a characteristic
//   write   device://local/ble/gatt/ADDR/SERVICE/CHAR  text | hex:0102…
//   write   device://local/ble/notify/ADDR/SERVICE/CHAR  on | off   subscribe; then stream it (read: the latest value)
//   session connect | disconnect device://local/ble/device/ADDR
//   write   device://local/ble/log  SECONDS | off              every advertisement to the card
//   write   device://local/ble/advertise  NAME | {"name","data","ibeacon","seconds"} | off
//   read    device://local/ble/hid ; write device://local/ble/hid/pair [SECONDS] | device://local/ble/hid/type TEXT | device://local/ble/hid/key COMBO | device://local/ble/hid/forget
//   write   settings://ble/power  on | off
//   streams device://local/ble/nearby, device://local/ble/rssi/ADDR, device://local/ble/notify/ADDR/SERVICE/CHAR
#include "../common.h"
#include "../ops_ble.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr char kUsage[] =
    "蓝牙规范：扫描附近设备 read target=device://local/ble/scan（可加 ?seconds=10）；设备详情 device://local/ble/device/地址，它的服务和特征 read device://local/ble/device/地址/services；"
    "读写特征 device://local/ble/gatt/地址/服务/特征（写入内容是文本或 hex:0102）；订阅通知 write device://local/ble/notify/地址/服务/特征 内容 on，之后程序用 stream 取；"
    "广播 write device://local/ble/advertise；当蓝牙键盘往电脑打字 write device://local/ble/hid/type（只能打英文、数字和符号），按键 write device://local/ble/hid/key 如 enter、cmd+space，"
    "配对电脑 write device://local/ble/hid/pair；状态 read device://local/ble/status。打开或关闭蓝牙用 write settings://ble/power 内容 on 或 off，重启后生效。";

constexpr char kPrefix[] = "device://local/ble/";
constexpr char kPower[] = "settings://ble/power";  // on | off, after a restart

// The part after device://local/ble/, without the query, split at '/'.
std::vector<String> parts_of(const String& target) {
    String rest = target.substring(strlen(kPrefix));
    const int q = rest.indexOf('?');
    if (q >= 0) rest = rest.substring(0, q);
    std::vector<String> parts;
    int from = 0;
    while (from <= static_cast<int>(rest.length())) {
        const int slash = rest.indexOf('/', from);
        const String part = url_decode(slash < 0 ? rest.substring(from) : rest.substring(from, slash));
        if (!part.isEmpty()) parts.push_back(part);
        if (slash < 0) break;
        from = slash + 1;
    }
    return parts;
}

uint32_t seconds_of(const String& text, uint32_t fallback) {
    const long n = text.toInt();
    return n > 0 ? static_cast<uint32_t>(n) : fallback;
}

bool is_off(String text) {
    text.trim();
    text.toLowerCase();
    return text == "off" || text == "stop" || text == "0" || text == "false" || text == "关" || text == "停止";
}

class BlePackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "read device://local/ble/scan|status|devices/list|device/地址，读写 device://local/ble/gatt/…、订阅 notify/…、广播 advertise、蓝牙键盘 hid/type|key|pair；开关 write settings://ble/power（on 或 off，重启生效）"; }

    const Manifest& manifest() const override {
        static const Manifest m{"ble", "蓝牙 BLE：扫描附近设备、广播、连接设备读写数据和订阅通知、当蓝牙键盘往电脑打字",
                                "蓝牙 bluetooth ble 扫描 附近 设备 信号 rssi 信标 beacon ibeacon 广播 连接 gatt 特征 服务 通知 订阅 "
                                "心率 温度计 手环 传感器 键盘 hid 打字 输入 配对 电脑",
                                {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        String target = json_str(arguments, "target");
        target.toLowerCase();
        if (target == kPower) return tool == "read" || tool == "write";
        return target.startsWith(kPrefix) && (tool == "read" || tool == "write" || tool == "session");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String target = json_str(arguments, "target");
        const String content = json_str(arguments, "content");
        if (target == kPower) return tool == "read" ? ble::status_text() : ble::set_enabled(!is_off(content));
        const auto p = parts_of(target);
        const String what = p.empty() ? String("status") : lower(p[0]);

        if (tool == "session") {
            const String action = json_str(arguments, "action");
            if (what != "device" || p.size() < 2) return "用法：session connect device://local/ble/device/地址";
            if (action == "disconnect") return ble::disconnect(p[1]);
            return ble::connect(p[1]);
        }
        if (tool == "read") {
            if (what == "status") return ble::status_text();
            if (what == "scan") return ble::scan(seconds_of(target_param(target, "seconds"), 5));
            if (what == "devices") return ble::devices_text();
            if (what == "device" && p.size() >= 3 && lower(p[2]) == "services") return ble::services(p[1]);
            if (what == "device" && p.size() >= 2) return ble::device_text(p[1]);
            if (what == "gatt" && p.size() >= 4) return ble::gatt_read(p[1], p[2], p[3]);
            if (what == "notify" && p.size() >= 4) return ble::notify_text(p[1] + "/" + p[2] + "/" + p[3]);
            if (what == "hid") return ble::hid_status();
            if (what == "nearby") {
                int count, strongest;
                if (!ble::nearby(count, strongest)) return ble::status_text();
                return "10 秒内听到 " + String(count) + " 个设备" + (count ? "，最强 " + String(strongest) + " dBm" : "（附近没在扫描时先 read device://local/ble/scan）");
            }
            if (what == "rssi" && p.size() >= 2) {
                int dbm;
                return ble::rssi(p[1], dbm) ? p[1] + "：" + String(dbm) + " dBm" : p[1] + " 最近 5 秒没有听到（先 read device://local/ble/scan）";
            }
            return help();
        }
        // write
        if (what == "log") return ble::scan_log(is_off(content) ? 0 : seconds_of(content, 300));
        if (what == "advertise") return is_off(content) ? ble::advertise_stop() : ble::advertise(content);
        if (what == "gatt" && p.size() >= 4) return ble::gatt_write(p[1], p[2], p[3], content);
        if (what == "notify" && p.size() >= 4) return ble::subscribe(p[1], p[2], p[3], !is_off(content));
        if (what == "hid" && p.size() >= 2) {
            const String action = lower(p[1]);
            if (action == "pair") return ble::hid_pair(seconds_of(content, 60));
            if (action == "type") return content.isEmpty() ? String("要输入什么？") : ble::hid_type(content);
            if (action == "key") return ble::hid_key(content);
            if (action == "forget") return ble::hid_forget();
        }
        return help();
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (tool == "write" && target == kPower) {
            return is_off(json_str(arguments, "content")) ? "关闭蓝牙（重启后生效）" : "打开蓝牙（重启后生效，约占 50 KB 内存）";
        }
        const auto p = parts_of(target);
        if (tool != "write" || p.empty()) return {};
        const String what = lower(p[0]);
        const String content = json_str(arguments, "content");
        if (what == "hid" && p.size() >= 2 && lower(p[1]) == "type") return "用蓝牙键盘在电脑上输入：\n" + content.substring(0, 200);
        if (what == "hid" && p.size() >= 2 && lower(p[1]) == "key") return "用蓝牙键盘在电脑上按：" + content;
        if (what == "hid" && p.size() >= 2 && lower(p[1]) == "pair") return "打开蓝牙键盘配对（期间附近的电脑都能配对）";
        if (what == "gatt" && p.size() >= 4) return "写入蓝牙设备 " + p[1] + " 的特征 " + p[3] + "：" + content.substring(0, 80);
        return {};
    }

    void triggers(std::vector<TriggerSpec>& out) const override {
        out.push_back({"device://local/ble/event/paired", "电脑配对了蓝牙键盘"});
        out.push_back({"device://local/ble/event/pair_timeout", "蓝牙键盘配对窗口超时，没有电脑连上"});
        out.push_back({"device://local/ble/event/connected", "已配对的电脑连上了蓝牙键盘"});
        out.push_back({"device://local/ble/event/disconnected", "电脑断开了蓝牙键盘"});
        out.push_back({"device://local/ble/event/log_done", "蓝牙扫描记录写完（data.lines 条数）"});
        out.push_back({"device://local/ble/event/advertise_done", "定时的蓝牙广播结束"});
    }

    bool watch(const String& target, String& normalized, String& error) override {
        if (!target.startsWith("device://local/ble/event/")) return false;
        std::vector<TriggerSpec> known;
        triggers(known);
        for (const auto& t : known) {
            if (target == t.target) {
                normalized = target;
                return true;
            }
        }
        error = "没有这个蓝牙事件：" + target;
        return true;
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"蓝牙", ble::running() ? "开" : "关（设置里打开，重启生效）", "扫一下附近的蓝牙设备"});
        if (ble::running()) out.push_back({"蓝牙键盘", ble::hid_status(), "在电脑上输入 hello world"});
    }

    void streams(std::vector<StreamSpec>& out) const override {
        if (!ble::running()) return;
        out.push_back({"device://local/ble/nearby", "count strongest", "", 1000, "10 秒内听到的蓝牙设备数和最强信号（dBm）；读的时候一直在扫描"});
        out.push_back({"device://local/ble/rssi/*", "rssi", "dBm", 500, "某个设备的信号强度：device://local/ble/rssi/地址（-50 很近，-90 很远）"});
        out.push_back({"device://local/ble/notify/*", "len b0 b1 b2 b3 b4 b5 b6", "", 100,
                       "已订阅特征的最新通知：device://local/ble/notify/地址/服务/特征（先 write 它 on），len 是长度，b0… 是前 7 个字节"});
    }

    String open_stream(const String& target) override {
        const auto p = parts_of(target);
        if (p.empty()) return "不是流";
        const String what = lower(p[0]);
        if (what == "nearby" || what == "rssi") {
            ble::want_scan(true);
            return {};
        }
        if (what == "notify" && p.size() >= 4) return {};
        return "不是流";
    }

    void close_stream(const String& target) override {
        const auto p = parts_of(target);
        if (!p.empty() && (lower(p[0]) == "nearby" || lower(p[0]) == "rssi")) ble::want_scan(false);
    }

    bool sample(const String& target, StreamValue& out) override {
        const String rest = target.substring(strlen(kPrefix));
        if (rest.startsWith("nearby")) {
            int count, strongest;
            if (!ble::nearby(count, strongest)) return false;
            out.v[0] = count;
            out.v[1] = strongest;
            out.count = 2;
            return true;
        }
        if (rest.startsWith("rssi/")) {
            int dbm;
            if (!ble::rssi(rest.substring(5), dbm)) return false;
            out.v[0] = dbm;
            out.count = 1;
            return true;
        }
        if (rest.startsWith("notify/")) {
            uint8_t bytes[7];
            size_t length = 0;
            if (!ble::notified(rest.substring(7), bytes, sizeof bytes, length)) return false;
            out.v[0] = length;
            for (size_t i = 0; i < 7; ++i) out.v[1 + i] = i < length ? bytes[i] : 0;
            out.count = 8;
            return true;
        }
        return false;
    }

private:
    static String lower(String s) {
        s.toLowerCase();
        return s;
    }

    static String help() {
        return "device://local/ble/ 用法：read device://local/ble/scan、device://local/ble/devices/list、device://local/ble/device/地址[/services]、device://local/ble/gatt/地址/服务/特征、device://local/ble/status；"
               "write device://local/ble/gatt/…（文本或 hex:）、device://local/ble/notify/…（on/off）、device://local/ble/advertise、device://local/ble/log、device://local/ble/hid/pair|type|key|forget、settings://ble/power";
    }
};

}  // namespace

Package* make_ble_package() { return new BlePackage(); }

}  // namespace vat::registry
