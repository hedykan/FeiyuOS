// pkg: the manager of everything on the card in /vat/things/lua (programs
// and packages): scan, list, enable (confirm the declared permissions),
// disable, delete; the online catalog, install, update and rollback
// (pkg_store).  Built in on purpose: it is what confirms code from the card
// and the network, so it cannot itself be a file on the card.
//   read  pkg://list · pkg://catalog[?url=目录地址]
//   write pkg://scan
//   write pkg://名字/enable | /disable | /delete
//   write pkg://名字/install · /update · /rollback   (confirmed)
//   write pkg://new?[sha256=…&]url=https://…/名字.lua (url last; confirmed; then enable)
#include "../common.h"
#include "../pkg_store.h"
#include "../prog_runtime.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr char kUsage[] =
    "卡上的程序与扩展包（pkg://）：read pkg://list 看全部及状态（已启用、待确认、已停用）和各自的权限；write pkg://scan "
    "重新扫描卡上的 .lua 文件（用户在电脑上改过卡后）；write pkg://名字/enable 启用（会让用户确认它的权限）、/disable 停用、/delete 删除。"
    "在线目录：read pkg://catalog 看可安装的程序和扩展包，write pkg://名字/install 安装、pkg://名字/update 更新（都会让用户确认），"
    "pkg://名字/rollback 退回上一版；从网址装 write pkg://new?url=网址。";

// "pkg://名字/enable" -> name, action
void split(const String& target, String& name, String& action) {
    String path = url_decode(target.substring(strlen("pkg://")));
    const int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);
    const int slash = path.lastIndexOf('/');
    name = slash < 0 ? path : path.substring(0, slash);
    action = slash < 0 ? String() : path.substring(slash + 1);
}

// "pkg://天气/install" -> verb "install", name "天气"; "pkg://new?url=…" -> verb "new", no name.
bool verb_of(const String& target, String& verb, String& name) {
    String path = url_decode(target.substring(strlen("pkg://")));
    const int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);
    if (path == "new") {
        verb = "new";
        name = String();
        return true;
    }
    for (const char* v : {"install", "update", "rollback"}) {
        if (path.endsWith(String("/") + v)) {
            verb = v;
            name = path.substring(0, path.length() - strlen(v) - 1);
            return true;
        }
    }
    return false;
}

class PkgPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "read pkg://list 看全部；write pkg://名字/enable 启用、/disable 停用、/delete 删除；write pkg://scan 重新扫描卡；在线目录 read pkg://catalog，write pkg://名字/install 安装、pkg://名字/update 更新"; }

    const Manifest& manifest() const override {
        static const Manifest m{"pkg", "卡上程序与扩展包的管理：扫描、列出、启用（确认权限）、停用、删除",
                                "扩展包 程序 管理 启用 停用 安装 卸载 扫描 权限 pkg", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("pkg://");
    }

    bool reversible(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return true;
        String verb, name;
        if (verb_of(json_str(arguments, "target"), verb, name)) return false;  // code from the network
        String action;
        split(json_str(arguments, "target"), name, action);
        return !programs::pkg_needs_confirm(name, action);
    }

    String confirm_summary(const String& tool, const cJSON* arguments) const override {
        if (tool != "write") return {};
        const String target = json_str(arguments, "target");
        String verb, name;
        if (verb_of(target, verb, name)) {
            return pkg_store::summary(verb, name, target_param(target, "url", true), target_param(target, "sha256"));
        }
        String action;
        split(target, name, action);
        return programs::pkg_summary(name, action);
    }

    String execute(const String& tool, const cJSON* arguments, bool approved) override {
        const String target = json_str(arguments, "target");
        if (tool == "read") {
            if (target_path(target) == "pkg://catalog") return pkg_store::catalog(target_param(target, "url", true));
            return programs::pkg_list();
        }
        if (target == "pkg://scan") return programs::scan();
        String verb, name;
        if (verb_of(target, verb, name)) {
            if (!approved) return "ERROR: 安装、更新、退回都需要用户确认";
            if (verb == "rollback") return programs::rollback(name);
            if (verb != "new") return pkg_store::install(name, verb == "update");
            const String url = target_param(target, "url", true);
            if (!url.isEmpty()) return pkg_store::install_url(url, target_param(target, "sha256"));
            return "ERROR: 从网址装用 write pkg://new?sha256=…&url=https://…/名字.lua（url 放最后）；目录里的用 write pkg://名字/install（先 read pkg://catalog）";
        }
        String action;
        split(target, name, action);
        if (name.isEmpty() || action.isEmpty()) return "ERROR: 用 write pkg://名字/enable、/disable 或 /delete；扫描用 write pkg://scan";
        return programs::pkg_action(name, action, approved);
    }
};

}  // namespace

Package* make_pkg_package() { return new PkgPackage(); }

}  // namespace vat::registry
