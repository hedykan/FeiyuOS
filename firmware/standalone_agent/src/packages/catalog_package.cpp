// Command catalogs: discovery-only packages that describe audited remote
// commands (search terms, risk, remote availability, --help).  They never
// execute anything; commands run through the ssh package's exec.
#include "../common.h"
#include "../ops_ssh.h"
#include "../registry.h"

namespace vat::registry {
namespace {

class CatalogPackage : public Package {
public:
    explicit CatalogPackage(Manifest manifest) : manifest_(std::move(manifest)) {}

    const Manifest& manifest() const override { return manifest_; }

    bool available(const String& profile) override {
        return ssh::program_available(profile, manifest_.availability_program);
    }

    const char* command_risk(const String& command) const override {
        std::vector<String> parts;
        int start = 0;
        while (start < static_cast<int>(command.length())) {
            int end = command.indexOf(' ', start);
            if (end < 0) end = command.length();
            parts.push_back(command.substring(start, end));
            start = end + 1;
        }
        if (parts.empty()) return "confirmation_required";
        const std::vector<String> args(parts.begin() + 1, parts.end());
        return ssh::is_read_only(parts[0], args) ? "read_only" : "confirmation_required";
    }

    const char* search_text(const String& command) const override {
        if (command == "docker ps") return "容器列表 运行状态";
        if (command == "docker logs") return "容器日志 输出";
        if (command == "docker inspect") return "容器详情 配置";
        if (command == "docker stats") return "容器资源 CPU 内存";
        if (command == "docker top") return "容器进程";
        if (command == "docker images") return "镜像列表";
        if (command == "docker info") return "Docker 信息";
        if (command == "docker version") return "Docker 版本";
        return "";
    }

private:
    Manifest manifest_;
};

}  // namespace

std::vector<Package*> make_catalog_packages() {
    return {
        new CatalogPackage({"linux", "Linux 系统只读巡检", "linux 系统 磁盘 内存 进程 日志 目录",
                            {"pwd", "whoami", "hostname", "uptime", "uname", "df", "free", "ps", "ls", "stat",
                             "head", "tail", "cat", "journalctl"},
                            "uname"}),
        new CatalogPackage({"git", "Git 仓库只读检查", "git 代码 仓库 提交 差异 状态",
                            {"git status", "git log", "git diff", "git show", "git grep", "git rev-parse"}, "git"}),
        new CatalogPackage({"docker", "Docker 容器查看和日志", "docker 容器",
                            {"docker ps", "docker logs", "docker inspect", "docker stats", "docker top",
                             "docker images", "docker info", "docker version"},
                            "docker"}),
        new CatalogPackage({"systemd", "systemd 服务状态", "systemd systemctl 服务 状态",
                            {"systemctl status", "systemctl show", "systemctl is-active", "systemctl is-enabled",
                             "systemctl list-units"},
                            "systemctl"}),
    };
}

}  // namespace vat::registry
