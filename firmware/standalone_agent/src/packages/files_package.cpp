// files: the device's own storage (TF card, or on-board flash when degraded),
// limited to /vat.  Keys, passwords and the SSH private key are never read or
// written here.
//   read  file:///vat/notes/todo.txt   a file (binary files: size only)
//   read  file:///vat/things/          a directory listing
//   write file:///vat/...              create / overwrite / append (confirmed)
// Paths without /vat are taken as relative to it: file:///todo.txt = /vat/todo.txt.
#include "../common.h"
#include "../registry.h"
#include "../storage.h"

namespace vat::registry {
namespace {

constexpr std::size_t kMaxRead = 32768;  // the worker stores long results as out#

String listing(const String& path) {
    cJSON* list = cJSON_Parse(storage::list_dir(path).c_str());
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
        cJSON_Delete(list);
        return path + " 是空目录";
    }
    String text = path + " 下 " + String(cJSON_GetArraySize(list)) + " 项：\n";
    int n = 0;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        const String name = json_str(entry, "name");
        if (name.startsWith(".")) continue;
        const bool dir = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "dir"));
        const double size = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "size"));
        text += String(++n) + ". " + name + (dir ? "/" : "  " + String(static_cast<long>(size)) + " 字节") + "\n";
        if (n >= 40) {
            text += "…（只列前 40 项）\n";
            break;
        }
    }
    cJSON_Delete(list);
    return text;
}

// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "本机 TF 卡文件用 file:///vat/路径（read 目录即列出，read 文件即读取，write 写入），不是 SSH 主机。";

class FilesPackage : public Package {
public:
    const char* usage() const override { return kUsage; }

    const Manifest& manifest() const override {
        static const Manifest m{"files", "本机 TF 卡文件：查看目录、读取、写入（限 /vat 下）",
                                "文件 TF 卡 SD 卡 存储卡 目录 文件夹 列出文件 读文件 写文件 保存到卡 本机文件", {}};
        return m;
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        return (tool == "read" || tool == "write") && json_str(arguments, "target").startsWith("file://");
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String path = storage::vat_path(json_str(arguments, "target"));
        const String why = storage::refused_path(path, tool == "write");
        if (!why.isEmpty()) return "ERROR: " + why;
        if (tool == "read") {
            if (!storage::exists(path)) return "ERROR: 没有这个文件或目录：" + path;
            const std::size_t size = storage::size(path);
            File probe = storage::fs().open(path);
            const bool dir = probe && probe.isDirectory();
            probe.close();
            if (dir) return listing(path);
            const String text = storage::read_range(path, 0, kMaxRead);
            if (static_cast<std::size_t>(strlen(text.c_str())) != text.length())
                return path + " 是二进制文件，" + String(size) + " 字节";
            return size > kMaxRead ? text + "\n…（文件共 " + String(size) + " 字节，只读了前 32 KB）" : text;
        }
        const String content = json_str(arguments, "content");
        const String mode = json_str(arguments, "mode");
        if (mode == "create" && storage::exists(path)) return "ERROR: 文件已存在：" + path + "（要覆盖请明确说覆盖）";
        const bool ok = mode == "append" ? storage::append(path, content) : storage::write_text(path, content);
        return ok ? "已写入 " + path + "（" + String(content.length()) + " 字节）" : "ERROR: 写入失败：" + path;
    }

    void hints(std::vector<Hint>& out) override {
        if (storage::degraded()) return;
        out.push_back({"文件", "", "卡上 /vat/things 里有什么"});
    }

    void resources(cJSON* list, int& remaining) override {
        add_resource(list, remaining, "file:///vat/", "files：本机 TF 卡，read 目录即列出、read 文件即读取，write 写文件");
    }
};

}  // namespace

Package* make_files_package() { return new FilesPackage(); }

}  // namespace vat::registry
