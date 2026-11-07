#pragma once

#include <cstdint>
#include <vector>

#include "common.h"

// Panels for the ui package: the small JSON the agent writes, checked against
// a field whitelist and size limits, and how a refresh result becomes lines.
// No device calls, so it builds on the host (tests/host).
//
//   {"title": "开发机负载", "lines": ["…"], "choices": ["重启服务", "看日志"],
//    "source": {"read": "sensor://gps/location", "every": "30s"}}
//
// source is one tool call: {"read": target} or {"exec": target} for a bare
// target, or {"tool": "exec", "arguments": {...}} in the tools' own shape.
namespace vat::ui_panel {

constexpr std::size_t kMaxJson = 2048;
constexpr std::size_t kMaxLines = 12;
constexpr std::size_t kMaxChoices = 8;
constexpr std::size_t kMaxText = 120;     // bytes per line or choice
constexpr uint32_t kMinEverySeconds = 10;

struct Spec {
    String title;
    std::vector<String> lines;
    std::vector<String> choices;
    String source_tool;    // "" (no refresh), or the tool name
    String source_args;    // its arguments, JSON
    uint32_t every_s = 0;  // 0: refresh once when opened
};

// false: `error` names the offending field.
bool parse(const String& json, Spec& out, String& error);
String to_json(const Spec& spec);
// A tool result as panel lines: split on newlines, blank lines dropped,
// each line cut to kMaxText, at most kMaxLines.
std::vector<String> lines_from(const String& text);
// A panel or app name: non-empty, ≤ 48 bytes, no / ? # & { } or quotes.
bool valid_name(const String& name);

}  // namespace vat::ui_panel
