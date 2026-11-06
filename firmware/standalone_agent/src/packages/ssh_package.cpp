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
constexpr char kUsage[] = "明确连接使用 session(action=connect,target=ssh://PROFILE)；普通 read/exec 不需要先连接。用户说出具体主机名（例如 devbox、开发机、生产机）时，就把该名称作为 profile，必须逐字保留 profile，不得翻译、改写或猜别名；不要询问它是否已保存，中文 profile 绝不能翻译成英文：开发机必须是 ssh://开发机，不能改成 devbox；若语音识别把中文 profile 输出为繁体字，只做繁体到简体规范化，例如生產機变为生产机；执行器会校验。只有完全没说主机名时才澄清。SSH 只是 target：主机为 ssh://PROFILE，远端文件为 ssh://PROFILE/ABSOLUTE_PATH，主机名与绝对路径之间只有一个斜杠，例如开发机的 /tmp/note.txt 是 ssh://开发机/tmp/note.txt。读取远端文件必须用 read；写文件必须用 write。普通非交互命令用 exec，并拆成 program 与 args，禁止拼成 shell 字符串；args 只放参数，不重复 program 本身，例如执行 whoami 是 program=whoami,args=[]，执行 df -h 是 program=df,args=[-h]；看到“执行/运行 Git status”等明确命令时必须用 exec，绝不能用 session。命令参数必须严格来自用户要求或 discover 结果，不要自行添加 --no-pager 等优化参数。用户指定工作目录时放入 exec.cwd，不要使用 cd 或 shell 拼接。常见且确定的只读命令可直接 exec；陌生命令、可用性或参数不确定时用 discover 查询。结合运维语境纠正英文命令听写：NGX、N G X 指 nginx；API 容器表示容器名 api，参数里不要保留‘容器’二字。docker logs 的选项放在容器名之前，例如最后50行必须是 args=[logs,--tail,50,api]。列出目录详细内容使用 ls -la 路径；创建目录使用 mkdir -p 路径；用户说删除目录时使用 rm -rf 路径。纠正语音路径格式：ETC/hostname、etc hostname 等应规范成 /etc/hostname。断开某主机连接时使用 session(action=disconnect,target=ssh://PROFILE)。连接主机或给主机配置免密登录都用 session(action=connect,target=ssh://主机)，设备在需要时请用户输入一次登录密码。";

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
