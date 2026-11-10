#pragma once

#include <cJSON.h>

#include <vector>

#include "common.h"
#include "lua_meta.h"

// The online catalog of programs and packages (packages/index.json in the
// FeiyuOS repository, made by tools/pkg_index.py).  Host-tested.
//
//   {"version": 1, "packages": [{"name": "发布", "version": "1.0", "kind": "package",
//     "about": "…", "package": "发布", "target": "release://", "usage": "…",
//     "uses": ["read https://api.github.com/repos/*"],
//     "url": "发布.lua", "sha256": "<64 hex>", "size": 812}]}
//
// "url" may be relative to the catalog's own address.  The fields copied from
// the file's header (uses, package, target, usage) are what the user confirms
// when installing: the downloaded file must declare exactly the same.
namespace vat::pkg_catalog {

struct Entry {
    String name;
    String version;
    String kind;  // program | package
    String about;
    String package;
    String target;
    String usage;
    std::vector<lua_meta::Use> uses;
    String url;     // absolute
    String sha256;  // lower-case hex
    std::size_t size = 0;
};

constexpr std::size_t kMaxCatalog = 16384;
constexpr std::size_t kMaxEntries = 64;

// "" when the catalog is well formed, else what is wrong (out is then empty).
// `base` is the catalog's address, for relative urls.
String parse(const String& json, const String& base, std::vector<Entry>& out);
const Entry* find(const std::vector<Entry>& entries, const String& name);
// The header an entry describes: compare lua_meta::digest() with the file's.
lua_meta::Header header_of(const Entry& entry);
// <0, 0, >0 comparing dotted versions number by number ("1.10" > "1.9").
int compare_versions(const String& a, const String& b);
// `url` against the catalog's address `base` ("x.lua" -> ".../packages/x.lua").
String resolve(const String& base, const String& url);

}  // namespace vat::pkg_catalog
