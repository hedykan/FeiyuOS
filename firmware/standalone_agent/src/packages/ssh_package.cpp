// ssh: remote files and commands over libssh.  Targets: ssh://PROFILE and
// ssh://PROFILE/ABSOLUTE_PATH; session(connect|disconnect|list).
#include "../common.h"
#include "../config.h"
#include "../ops_ssh.h"
#include "../ops_tui.h"
#include "../registry.h"

namespace vat::registry {
namespace {

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "SSH：主机 ssh://主机名，远端文件 ssh://主机名/绝对路径。读文件 read，写、覆盖、追加文件都用 write（不用 echo 重定向）；命令一律 exec：program 是命令名，args 是参数数组（不含命令名，不拼 shell 字符串，不用 cd，工作目录放 cwd），参数只来自用户的话或 discover，不自己加 --no-pager 之类；选项写在操作对象前（docker logs --tail 50 api）；列出目录内容 ls -la，看当前所在目录 pwd，建目录 mkdir -p，删目录 rm -rf。主机上有没有某个程序拿不准时先 discover（scope=capabilities，target 写 ssh://主机），某个命令怎么用拿不准时 discover（scope=help），不要试着执行。read、exec 不用先连接；连接或配置免密登录 session(connect, ssh://主机)，断开 session(disconnect, ssh://主机)。用户对主机的叫法（中文、英文都算，如“开发机”）就是主机名，即使不在可见 profile 列表里也原样写成 ssh://主机名，由设备校验；不为确认主机去 discover，不问主机是否存在或已保存。只在完全没说主机时澄清。";

class SshPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"ssh", "SSH 连接、远端文件和命令", "ssh 远程 主机 文件 命令", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        const String target = json_str(arguments, "target");
        if (tool == "read" || tool == "write" || tool == "exec") return target.startsWith("ssh://");
        if (tool != "session") return false;
        const String action = json_str(arguments, "action");
        return (action == "connect" || action == "disconnect" || action == "list") &&
               (target.isEmpty() || target.startsWith("ssh://"));
    }

    // Two operations need a password typed on the device:
    //  - sudo when the host does not allow it without one (probed with sudo -n);
    //  - connect when the device key is not accepted yet: the password is used
    //    once to install the key, after which the host is password-free.
    String needs_secret(const String& tool, const cJSON* arguments) override {
        const String profile = profile_of(json_str(arguments, "target"));
        const SshProfile* entry = config::find_profile(profile);
        if (!entry) return {};
        if (tool == "exec" && json_str(arguments, "program") == "sudo") {
            return ssh::sudo_needs_password(profile) ? profile + " · " + entry->user + " 的 sudo 密码" : String();
        }
        if (tool == "session" && json_str(arguments, "action") == "connect" && !ssh::key_accepted(profile)) {
            return profile + " · " + entry->user + " 的登录密码（仅用一次，配置免密登录）";
        }
        return {};
    }

    String execute_secret(const String& tool, const cJSON* arguments, bool approved, const String& secret) override {
        if (tool == "exec") return ssh::tool_exec(arguments, approved, secret);
        const String profile = profile_of(json_str(arguments, "target"));
        const String installed = ssh::install_key(profile, secret);
        if (!installed.startsWith("已为")) return installed;
        const String shell = tui::ensure_shell(profile);
        return shell.isEmpty() ? installed + "，终端会话 vat-shell 已就绪（SSH 页）" : installed + "。" + shell;
    }

    void hints(std::vector<Hint>& out) override {
        const String host = tui::has_current() ? tui::current().profile : config::get().default_profile;
        out.push_back({"主机", host, "连接" + host});
        out.push_back({"远程命令", "", "在" + host + "看下磁盘"});
        out.push_back({"远程文件", "", "读" + host + "的 /etc/hostname"});
        out.push_back({"免密登录", "", "给新主机配置免密登录（连接时输入一次密码）"});
    }

    String execute(const String& tool, const cJSON* arguments, bool approved) override {
        if (tool == "read") return ssh::tool_read(arguments);
        if (tool == "write") return ssh::tool_write(arguments);
        if (tool == "exec") return ssh::tool_exec(arguments, approved);
        const String action = json_str(arguments, "action");
        if (action == "list") return ssh::states_text();
        const String profile = profile_of(json_str(arguments, "target"));
        if (profile.isEmpty()) return "需要明确的 ssh://PROFILE，已知: " + config::profile_names();
        if (action == "disconnect") {
            tui::forget(profile);
            return ssh::disconnect(profile);
        }
        const String connected = ssh::connect(profile);
        if (!connected.startsWith("已连接")) return connected;
        const String shell = tui::ensure_shell(profile);
        return shell.isEmpty() ? connected + "，终端会话 vat-shell 已就绪（SSH 页）" : connected + "。" + shell;
    }

    void resources(cJSON* list, int& remaining) override {
        for (const auto& profile : config::get().profiles) {
            if (remaining <= 0) return;
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "target", ("ssh://" + profile.id).c_str());
            cJSON_AddStringToObject(item, "kind", "ssh_profile");
            cJSON_AddBoolToObject(item, "connected", ssh::connected(profile.id));
            cJSON_AddItemToArray(list, item);
            --remaining;
        }
    }
};

}  // namespace

Package* make_ssh_package() { return new SshPackage(); }

}  // namespace vat::registry
