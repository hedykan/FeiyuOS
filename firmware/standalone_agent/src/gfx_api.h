#pragma once

// The gfx functions: M5GFX's drawing calls by their own names and arguments
// (docs/V075_PLAN.zh-CN.md G2b), as one table.  The Lua functions (gfx.*),
// the ops of gfx://frame and of canvases, the device's executor (gfx_exec),
// read gfx://help and the host tests all come from it: a function is a row.
// Portable (host-tested): no M5GFX here.
#include <cJSON.h>

#include <cstdint>
#include <vector>

#include "common.h"

namespace vat::gfx_api {

// Parameter types, one letter each:
//   i integer   f number   c colour (#RRGGBB, a name, or an RGB565 number)
//   s text      F font     b boolean   d text datum   S gradient style
//   M 6 numbers (affine matrix)   u one character (text or code point)
//   B bitmap bytes (1 bit per pixel)   D RGB565 pixels   P an image file under /vat/
// `params` lists them; what follows '[' is optional; '|' separates overloads.
enum class Group : uint8_t { Draw, State, Query };

struct Fn {
    const char* name;
    const char* params;
    const char* example;  // Lua arguments that work (host tests, E2E, gfx://help)
    Group group;
};

const Fn* table();
std::size_t count();
const Fn* find(const String& name);
int index_of(const Fn* fn);

// The parameter letters of `overload` of `fn` ("iiii" + optional "c" -> "iiiic").
String overload_params(const Fn& fn, int overload);

constexpr std::size_t kMaxText = 120;     // bytes of text in one call
constexpr std::size_t kMaxBytes = 32768;  // bitmap, pixel and image bytes in one frame
constexpr double kMinCoord = -960;
constexpr double kMaxCoord = 1920;

// Checks `args` (a JSON array of the call's arguments, without the name)
// against `fn`'s overloads.  `out` gets them as text: numbers, colours as
// #RRGGBB, fonts and datums by name, bitmaps and pixels as hex; a "{t}"-like
// placeholder stands where a number or colour goes and is filled before
// drawing.  `bytes` adds the bitmap and pixel bytes of this call.
bool check(const Fn& fn, const cJSON* args, std::vector<String>& out, int& overload, std::size_t& bytes,
           String& error);

// Fonts and datums by name (the executor maps them to M5GFX's).
const char* const* font_names(std::size_t& n);
int datum_of(const String& name);  // -1: unknown
uint16_t rgb565(uint32_t rgb);

// read gfx://help: every function with its parameters and an example.
String help();

}  // namespace vat::gfx_api
