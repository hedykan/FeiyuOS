// ir: the IR emitter (see ops_ir, ops_ac); addresses per docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §5.  No learning: built-in TV codes and
// the IRext AC database on the card; the person says whether a send worked.
//   write device://local/ir/客厅电视/power       content: brand the first time (samsung/lg/sony/philips), may be empty
//   write device://local/ir/客厅空调/ac          content: the state or the change, brand the first time (奥克斯 开 制冷 26)
//   write device://local/ir/客厅电视/confirm     content: yes | no   (no tries the next code set / remote)
//   write device://local/ir/send                content: "NEC 0x04 0x08"
//   read  device://local/ir/list
#include "../common.h"
#include "../ops_ac.h"
#include "../ops_ir.h"
#include "../registry.h"

namespace vat::registry {
namespace {


String canonical_action(String action) {
    action.toLowerCase();
    for (const char* a : {"power", "on", "off", "电源", "开关", "开机", "关机", "打开", "关闭", "toggle"}) {
        if (action == a) return "power";
    }
    for (const char* a : {"volume_up", "vol_up", "volup", "音量+", "音量加", "大声"}) {
        if (action == a) return "volume_up";
    }
    for (const char* a : {"volume_down", "vol_down", "voldown", "音量-", "音量减", "小声"}) {
        if (action == a) return "volume_down";
    }
    if (action == "mute" || action == "静音") return "mute";
    for (const char* a : {"ac", "空调", "aircon", "air_conditioner"}) {
        if (action == a) return "ac";
    }
    for (const char* a : {"try", "试码", "选码", "换码"}) {
        if (action == a) return "try";
    }
    return action;
}

bool yes(String answer) {
    answer.toLowerCase();
    for (const char* no : {"no", "false", "没", "不", "无"}) {
        if (answer.indexOf(no) >= 0) return false;
    }
    return true;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "红外遥控电视用 write target=device://local/ir/电器名/动作（power、volume_up、volume_down、mute），电器名逐字保留，首次 content 填品牌，没说品牌留空。空调用 write target=device://local/ir/电器名/ac，content 写要的状态或变化，如 奥克斯 开 制冷 26 自动风、关、+1、除湿、高风，首次带品牌，自动模式写 mode=auto，自动风写 fan=auto；用户说生效或没反应时 write device://local/ir/电器名/confirm，content 为 yes 或 no；用户要换到上一套、下一套或第几套码时 write device://local/ir/电器名/try，content 为 上一套、下一套、重发 或编号数字；列出红外电器用 read device://local/ir/list。查到某设备的红外码（如联网搜到后）用 write target=device://local/ir/send，content 为「协议 地址 命令」，协议支持 NEC/SAMSUNG/SONY/RC5，如 content 为 NEC 0x04 0x08。";

class IrPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "电视 write device://local/ir/电器名/power（电器名整段照原话，如 客厅电视；开和关都是 power）、volume_up、volume_down、mute，第一次 content 写品牌（没说就留空）、device://local/ir/电器名/ac（空调），用户说生效或没反应 write device://local/ir/电器名/confirm（yes 或 no；no 时设备自动换下一套码重发，不用再发一次），换码 device://local/ir/电器名/try，发已知的码 device://local/ir/send，列出 read device://local/ir/list"; }
    // Built-in named TV codes are reversible (auto); a raw device://local/ir/send carries an
    // arbitrary code (e.g. searched online), so it needs confirmation.
    bool reversible(const String&, const cJSON* arguments) const override {
        return url_decode(json_str(arguments, "target").substring(strlen("device://local/ir/"))) != "send";
    }

    const Manifest& manifest() const override {
        static const Manifest m{"ir", "红外遥控：电视（电源、音量、静音，内置三星/LG/索尼/飞利浦码）和空调（开关、模式、温度、风速，TF 卡上的 IRext 码库），人工确认是否生效",
                                "红外 遥控 遥控器 电视 开电视 关电视 音量 静音 空调 开空调 关空调 温度 制冷 制热 除湿 风速 IR 电器", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("device://local/ir/");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String rest = url_decode(json_str(arguments, "target").substring(strlen("device://local/ir/")));
        if (tool == "read" || rest == "list" || rest.isEmpty()) return ir::list_text();
        const String content = json_str(arguments, "content");
        if (rest == "send") return ir::send_raw(content);
        const int slash = rest.indexOf('/');
        const String name = slash < 0 ? rest : rest.substring(0, slash);
        const String action = slash < 0 ? String("power") : canonical_action(rest.substring(slash + 1));
        if (action == "confirm") return ac::is_ac(name) ? ac::confirm(name, yes(content)) : ir::confirm(name, yes(content));
        if (action == "ac") return ac::send(name, content);
        if (action == "try") {
            return ac::is_ac(name) ? ac::choose(name, content) : "ERROR: 「" + name + "」不是空调，换码只用于空调";
        }
        return ir::send(name, action, content);
    }

    void hints(std::vector<Hint>& out) override {
        out.push_back({"红外", "", "用红外打开客厅电视"});
        out.push_back({"空调", "", "把客厅空调开到 26 度制冷"});
    }

    void resources(cJSON* list, int& remaining) override {
        static const char* const items[][2] = {
            {"device://local/ir/list", "ir：read 列出红外电器与验证状态"},
            {"device://local/ir/电器名/power", "ir：write 发电源码（也可 volume_up/volume_down/mute），首次 content 填品牌"},
            {"device://local/ir/电器名/ac", "ir：write 控制空调，content 为状态或变化（奥克斯 开 制冷 26 / 关 / +1 / 除湿 / 高风），首次带品牌"},
            {"device://local/ir/电器名/confirm", "ir：用户说生效或没反应时 write，content 为 yes/no"},
            {"device://local/ir/电器名/try", "ir：空调换到指定的码，write content 为 上一套 / 下一套 / 重发 / 编号数字"},
            {"device://local/ir/send", "ir：write 发任意红外码（需确认），content 为「协议 地址 命令」如 NEC 0x04 0x08"},
        };
        for (const auto& item : items) {
            add_resource(list, remaining, item[0], item[1]);
        }
    }
};

}  // namespace

Package* make_ir_package() { return new IrPackage(); }

}  // namespace vat::registry
