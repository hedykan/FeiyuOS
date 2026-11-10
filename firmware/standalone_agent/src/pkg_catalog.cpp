#include "pkg_catalog.h"

namespace vat::pkg_catalog {
namespace {

bool hex64(const String& text) {
    if (text.length() != 64) return false;
    for (std::size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

String resolve(const String& base, const String& url) {
    if (url.indexOf("://") > 0) return url;
    String dir = base;
    const int query = dir.indexOf('?');
    if (query >= 0) dir = dir.substring(0, query);
    std::size_t cut = 0;  // after the last slash (the host shim has no lastIndexOf)
    for (std::size_t i = 0; i < dir.length(); ++i) {
        if (dir[i] == '/') cut = i + 1;
    }
    return dir.substring(0, cut) + url;
}

String parse(const String& json, const String& base, std::vector<Entry>& out) {
    out.clear();
    if (json.length() > kMaxCatalog) return "目录超过 16 KB";
    cJSON* root = cJSON_Parse(json.c_str());
    const cJSON* list = json_arr(root, "packages");
    if (!list) {
        cJSON_Delete(root);
        return "不是目录（没有 packages 列表）";
    }
    String error;
    int index = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        ++index;
        Entry e;
        e.name = json_str(item, "name");
        e.version = json_str(item, "version");
        e.kind = json_str(item, "kind", "program");
        e.about = json_str(item, "about");
        e.package = json_str(item, "package");
        e.target = json_str(item, "target");
        e.usage = json_str(item, "usage");
        e.url = resolve(base, json_str(item, "url"));
        e.sha256 = json_str(item, "sha256");
        e.sha256.toLowerCase();
        const cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "size");
        e.size = cJSON_IsNumber(size) && size->valuedouble > 0 ? static_cast<std::size_t>(size->valuedouble) : 0;
        const String where = "第 " + String(index) + " 项（" + e.name + "）：";
        const cJSON* use = nullptr;
        cJSON_ArrayForEach(use, json_arr(item, "uses")) {
            String text = cJSON_IsString(use) ? String(use->valuestring) : String();
            text.trim();
            const int space = text.indexOf(' ');
            String tool = space < 0 ? text : text.substring(0, space);
            String target = space < 0 ? String() : text.substring(space + 1);
            tool.trim();
            target.trim();
            if (tool.isEmpty() || target.isEmpty()) error = where + "uses 每项要写成「动作 目标」";
            e.uses.push_back({tool, target});
        }
        if (error.isEmpty() && !lua_meta::check_name(e.name).isEmpty()) error = where + lua_meta::check_name(e.name);
        if (error.isEmpty() && !e.url.startsWith("https://") && !e.url.startsWith("http://"))
            error = where + "url 要是 http(s)://，或相对目录的路径";
        if (error.isEmpty() && !hex64(e.sha256)) error = where + "sha256 要是 64 位十六进制";
        if (error.isEmpty() && (e.size == 0 || e.size > lua_meta::kMaxSource)) error = where + "size 要在 1 字节到 16 KB 之间";
        if (error.isEmpty() && e.kind != "program" && e.kind != "package") error = where + "kind 只能是 program 或 package";
        if (error.isEmpty() && e.kind == "package" && (e.package.isEmpty() || e.target.indexOf("://") <= 0))
            error = where + "扩展包要有 package 和 target";
        if (error.isEmpty() && e.kind == "program" && !e.package.isEmpty()) error = where + "程序不能有 package";
        if (error.isEmpty() && find(out, e.name)) error = where + "名字重复";
        if (!error.isEmpty()) break;
        if (out.size() >= kMaxEntries) break;
        out.push_back(e);
    }
    cJSON_Delete(root);
    if (!error.isEmpty()) out.clear();
    return error;
}

const Entry* find(const std::vector<Entry>& entries, const String& name) {
    for (const auto& e : entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

lua_meta::Header header_of(const Entry& entry) {
    lua_meta::Header h;
    h.package = entry.package;
    h.target = entry.target;
    h.usage = entry.usage;
    h.uses = entry.uses;
    return h;
}

int compare_versions(const String& a, const String& b) {
    std::size_t i = 0, j = 0;
    while (i < a.length() || j < b.length()) {
        long x = 0, y = 0;
        for (; i < a.length() && a[i] != '.'; ++i) {
            if (a[i] >= '0' && a[i] <= '9') x = x * 10 + (a[i] - '0');
        }
        for (; j < b.length() && b[j] != '.'; ++j) {
            if (b[j] >= '0' && b[j] <= '9') y = y * 10 + (b[j] - '0');
        }
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

}  // namespace vat::pkg_catalog
