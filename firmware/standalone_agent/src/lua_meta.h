#pragma once

#include <cstdint>
#include <vector>

#include "common.h"

// The header of a .lua program or package on the card: its leading comment
// lines of the form "-- @key value".  Host-tested.
//
//   -- @about 早上查天气，下雨就提醒带伞
//   -- @input city 深圳            (a parameter and its default; none: required)
//   -- @uses read place://weather  (a call that needs the user's confirmation)
//   -- @uses write device://local/display/*
//   -- @uses gfx                   (a package's Lua functions: gfx.fillRect …; nothing to confirm)
//   -- @version 1.2
//   -- @package weather            (a package: it serves @target for others)
//   -- @target weather://
//   -- @usage 查天气用 read weather://now?city=城市名
//   -- @keywords 天气 气温 下雨
namespace vat::lua_meta {

struct Use {
    String tool;    // read | write | exec | observe | input | session
    String target;  // exact, or a prefix ending in '*'
};

struct Input {
    String name;
    String fallback;
    bool required = true;
};

struct Header {
    String about;
    String version;
    String package;  // non-empty: a package
    String target;   // the scheme a package serves ("weather://")
    String usage;
    String keywords;
    std::vector<Input> inputs;
    std::vector<Use> uses;
    std::vector<String> libraries;  // packages whose Lua functions the script uses (@uses gfx)
};

constexpr std::size_t kMaxName = 48;     // bytes (16 Chinese characters)
constexpr std::size_t kMaxSource = 16384;

// "" when the header is well formed, else what is wrong, with the line.
String parse(const String& source, Header& out);
// Whether the header declares a call needing the user's confirmation once
// (anything but read: reads that Policy would ask about are declared too, and
// confirmed the same way).
bool needs_confirm(const Header& header);
// Whether `uses` covers `tool` on `target`: "ssh://开发机" covers
// ssh://开发机, ssh://开发机/… and ssh://开发机?…; "https://a.com/*" covers
// anything starting with https://a.com/.
bool covers(const std::vector<Use>& uses, const String& tool, const String& target);
// What was confirmed (the uses, and a package's target and usage): changes when they do.
uint32_t digest(const Header& header);
String describe(const Use& use);  // "执行 ssh://开发机"
// "" when `name` can name a program or package file.
String check_name(const String& name);

}  // namespace vat::lua_meta
