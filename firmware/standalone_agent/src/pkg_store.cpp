#include "pkg_store.h"

#include <mbedtls/sha256.h>

#include "common.h"
#include "lua_meta.h"
#include "lua_vm.h"
#include "net.h"
#include "pkg_catalog.h"
#include "prog_runtime.h"
#include "storage.h"

namespace vat::pkg_store {
namespace {

constexpr char kDefaultCatalog[] = "https://raw.githubusercontent.com/hedykan/FeiyuOS/main/packages/index.json";
constexpr char kCatalogFile[] = "/vat/cache/pkg/catalog.json";
constexpr char kSourceFile[] = "/vat/cache/pkg/catalog.url";
constexpr char kDownload[] = "/vat/cache/pkg/download.lua";

String sha256_file(const String& path) {
    File file = storage::fs().open(path, FILE_READ);
    if (!file) return {};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buffer[512];
    int n;
    while ((n = file.read(buffer, sizeof(buffer))) > 0) mbedtls_sha256_update(&ctx, buffer, n);
    file.close();
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    static const char kHex[] = "0123456789abcdef";
    String out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

String host_of(const String& url) {
    const int start = url.indexOf("://") + 3;
    const int end = url.indexOf('/', start);
    return url.substring(start, end < 0 ? url.length() : end);
}

// GET `url` into `path`; "" or why not.
String fetch(const String& url, const String& path, std::size_t limit) {
    if (net::state() != net::WifiState::Connected) return "Wi-Fi 未连接";
    storage::remove(path);
    if (!storage::ensure_parent(path)) return "卡上建不了目录";
    net::Request request;
    request.url = url;
    request.save_path = path;
    request.max_body = 512;
    request.timeout_ms = 30000;
    const net::HttpResult result = net::request(request);
    String error;
    if (result.status <= 0) error = "下载失败：" + result.error;
    else if (!result.ok()) error = "HTTP " + String(result.status);
    else if (storage::size(path) > limit) error = "文件超过 " + String(static_cast<int>(limit / 1024)) + " KB";
    if (!error.isEmpty()) storage::remove(path);
    return error;
}

String load_catalog(std::vector<pkg_catalog::Entry>& entries, String& source) {
    source = storage::read_text(kSourceFile, 512);
    source.trim();
    const String text = storage::read_text(kCatalogFile, pkg_catalog::kMaxCatalog);
    if (text.isEmpty() || source.isEmpty()) return "还没有读过在线目录，先 read pkg://catalog";
    return pkg_catalog::parse(text, source, entries);
}

// Downloads `url` and checks it; "" with `header` and `source` filled, or why not.
String download_checked(const String& url, const String& sha256, std::size_t size, lua_meta::Header& header) {
    String error = fetch(url, kDownload, lua_meta::kMaxSource);
    if (error.isEmpty() && size && storage::size(kDownload) != size) {
        error = "大小不对（目录写 " + String(static_cast<unsigned>(size)) + " 字节，下载到 " +
                String(static_cast<unsigned>(storage::size(kDownload))) + "）";
    }
    if (error.isEmpty() && !sha256.isEmpty() && sha256_file(kDownload) != sha256) {
        error = "SHA-256 校验失败：文件被改过或没下载完整，未安装";
    }
    if (error.isEmpty()) {
        const String source = storage::read_text(kDownload, lua_meta::kMaxSource);
        error = lua_meta::parse(source, header);
        if (!error.isEmpty()) error = "声明有误：" + error;
        else error = luavm::compile(source, "download");
    }
    if (!error.isEmpty()) storage::remove(kDownload);
    return error;
}

String uses_text(const std::vector<lua_meta::Use>& uses) {
    String out;
    for (std::size_t i = 0; i < uses.size(); ++i) out += (i ? "；" : "") + lua_meta::describe(uses[i]);
    return out;
}

String kind_text(const pkg_catalog::Entry& e) {
    return e.kind == "package" ? "扩展包 " + e.target : String("程序");
}

}  // namespace

String catalog(const String& url_in) {
    const String url = url_in.isEmpty() ? String(kDefaultCatalog) : url_in;
    if (!url.startsWith("https://") && !url.startsWith("http://")) return "ERROR: 目录地址要是 http(s)://";
    const String fresh = String(kCatalogFile) + ".new";
    String error = fetch(url, fresh, pkg_catalog::kMaxCatalog);
    if (!error.isEmpty()) return "ERROR: 读在线目录失败：" + error;
    std::vector<pkg_catalog::Entry> entries;
    error = pkg_catalog::parse(storage::read_text(fresh, pkg_catalog::kMaxCatalog), url, entries);
    if (!error.isEmpty()) {
        storage::remove(fresh);
        return "ERROR: 目录格式不对：" + error;
    }
    if (!storage::rename(fresh, kCatalogFile) || !storage::write_text(kSourceFile, url)) return "ERROR: 目录没能存到卡上";
    String out = "在线目录（" + host_of(url) + "，" + String(static_cast<int>(entries.size())) + " 个）：\n";
    for (const auto& e : entries) {
        out += "- " + e.name + " v" + e.version + " [" + kind_text(e) + "]";
        if (!e.about.isEmpty()) out += "：" + e.about;
        if (!e.uses.empty()) out += "；权限：" + uses_text(e.uses);
        lua_meta::Header have;
        if (!programs::installed(e.name, have)) out += "；未安装（write pkg://install/" + e.name + "）";
        else if (pkg_catalog::compare_versions(e.version, have.version) > 0)
            out += "；已安装 v" + have.version + "，可更新（write pkg://update/" + e.name + "）";
        else out += "；已安装";
        out += "\n";
    }
    out.trim();
    return out;
}

String summary(const String& verb, const String& name, const String& url, const String& sha256) {
    if (verb == "rollback") return "退回「" + name + "」到保留的上一版";
    if (!url.isEmpty()) {
        return "从 " + utf8_head(url, 120) + " 下载程序" +
               (sha256.isEmpty() ? String("（未提供 SHA-256）") : "（SHA-256 " + sha256.substring(0, 12) + "…）") +
               (url.startsWith("http://") ? String("，未加密") : String()) + "；装好后是「待确认」，看过权限再启用";
    }
    std::vector<pkg_catalog::Entry> entries;
    String source;
    const String error = load_catalog(entries, source);
    const pkg_catalog::Entry* e = error.isEmpty() ? pkg_catalog::find(entries, name) : nullptr;
    if (!e) return (verb == "update" ? "更新「" : "安装「") + name + "」（目录里没有，先 read pkg://catalog）";
    String out = (verb == "update" ? "更新「" : "安装「") + name + "」v" + e->version + " 自 " + host_of(source) + " [" +
                 kind_text(*e) + "]";
    if (!e->about.isEmpty()) out += "（" + e->about + "）";
    if (!e->usage.isEmpty()) out += "；告诉肥鱼：" + utf8_head(e->usage, 160);
    lua_meta::Header have;
    if (verb == "update" && programs::installed(name, have)) {
        std::vector<lua_meta::Use> added;  // declared now, not by the installed version
        for (const auto& u : e->uses) {
            bool same = false;
            for (const auto& h : have.uses) same = same || (h.tool == u.tool && h.target == u.target);
            if (!same) added.push_back(u);
        }
        out += added.empty() ? String("；权限不变") : "；新增权限：" + uses_text(added);
    } else if (!e->uses.empty()) {
        out += "；允许它：" + uses_text(e->uses);
    }
    return out;
}

String install(const String& name, bool update) {
    std::vector<pkg_catalog::Entry> entries;
    String source;
    String error = load_catalog(entries, source);
    if (!error.isEmpty()) return "ERROR: " + error;
    const pkg_catalog::Entry* e = pkg_catalog::find(entries, name);
    if (!e) return "ERROR: 目录里没有「" + name + "」（read pkg://catalog 看全部）";
    lua_meta::Header have;
    const bool present = programs::installed(name, have);
    if (!update && present) return "ERROR: 「" + name + "」已经装了，更新用 write pkg://update/" + name;
    if (update && !present) return "ERROR: 还没装「" + name + "」，用 write pkg://install/" + name;
    if (update && pkg_catalog::compare_versions(e->version, have.version) <= 0) {
        return "「" + name + "」已是最新（v" + have.version + "）";
    }
    lua_meta::Header header;
    error = download_checked(e->url, e->sha256, e->size, header);
    if (!error.isEmpty()) return "ERROR: " + error;
    if (lua_meta::digest(header) != lua_meta::digest(pkg_catalog::header_of(*e)) ||
        header.package.isEmpty() != (e->kind == "program")) {
        storage::remove(kDownload);
        return "ERROR: 文件里的声明（权限、负责的 scheme 或说明）和目录写的不一致，未安装";
    }
    error = programs::adopt(name, kDownload, header, true);
    if (!error.isEmpty()) {
        storage::remove(kDownload);
        return "ERROR: " + error;
    }
    return String(update ? "已更新「" : "已安装「") + name + "」v" + header.version + "，SHA-256 校验通过，已启用" +
           (update ? "；上一版留着，write pkg://rollback/" + name + " 可退回" : String());
}

String install_url(const String& url, const String& sha256_in) {
    String sha256 = sha256_in;
    sha256.toLowerCase();
    if (!url.startsWith("https://") && !url.startsWith("http://")) return "ERROR: 地址要是 http(s)://";
    String file = url;
    const int query = file.indexOf('?');
    if (query >= 0) file = file.substring(0, query);
    file = url_decode(file.substring(file.lastIndexOf('/') + 1));
    if (!file.endsWith(".lua")) return "ERROR: 地址要指向一个 .lua 文件";
    const String name = file.substring(0, file.length() - 4);
    const String bad = lua_meta::check_name(name);
    if (!bad.isEmpty()) return "ERROR: 文件名不能做程序名：" + bad;
    lua_meta::Header header;
    const String error = download_checked(url, sha256, 0, header);
    if (!error.isEmpty()) return "ERROR: " + error;
    const String placed = programs::adopt(name, kDownload, header, false);
    if (!placed.isEmpty()) {
        storage::remove(kDownload);
        return "ERROR: " + placed;
    }
    String out = "已下载「" + name + "」" + (header.version.isEmpty() ? String() : " v" + header.version) +
                 (sha256.isEmpty() ? String("（未校验 SHA-256）") : String("（SHA-256 校验通过）")) +
                 "，状态：待确认；";
    if (!header.uses.empty()) out += "它要求：" + uses_text(header.uses) + "；";
    return out + "write pkg://" + name + "/enable 启用";
}

}  // namespace vat::pkg_store
