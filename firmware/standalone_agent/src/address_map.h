#pragma once

#include <Arduino.h>

// Retired resource addresses rewritten to the current ones
// (docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §5.6).  Portable: tested on the host.
namespace vat::address {

// The card's data is rewritten once per version (prog_runtime: programs,
// approved.json, triggers.json, ui apps); bump it with a new §5.6 row.
constexpr int kVersion = 2;

// Every retired address in `text` (a Lua source, JSON, …) in its current form.
String rewrite(const String& text);

// The current form of a retired target, or "" when `target` is not retired.
// A call to a retired address fails with this instead of being guessed at.
String retired(const String& target);

}  // namespace vat::address
