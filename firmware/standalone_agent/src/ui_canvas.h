#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "common.h"

// Canvases for the ui package: drawing operations close to the M5GFX API,
// written by the agent as JSON and checked against a whitelist.  Parsing,
// limits and filling in live data are here (host-tested); ui_overlay draws.
//
//   {"ops": [["fillScreen", "#101820"],
//            ["fillRoundRect", 10, 10, 100, 40, 6, "#3FB8AF"],
//            ["drawString", "{1}", 120, 60, {"size": 3, "color": "white", "datum": "middle_center"}],
//            ["progressBar", 10, 110, 220, 10, "{2}", "#3FB8AF"]],
//    "source": {"read": "device://local/battery", "every": "30s"}}
//
// "{1}", "{2}" … in any argument are lines of the source's latest result;
// "{1#}" is the first number in line 1 (for big numbers and bars).
//
// Rotation: ["rotate", angle, cx, cy] turns the shapes after it about
// (cx, cy); ["rotate", 0, 0, 0] stops.  Animation: "animate": {"every":
// "100ms", "frames": 36, "step": 10} redraws every 100 ms with "{t}" =
// frame × step (frames 0: until closed), e.g. ["rotate", "{t}", 120, 67].
//
// Variables: "vars": {"x": 10, "vx": 3} and "update": "x = x + vx  if x >
// 230 or x < 10 then vx = -vx end" (Lua, run before every frame on a VM
// kept while the canvas is open: lua_vm Script; t is the frame × step;
// vat.stream reads sensors) and "{x}" in any argument is the value.  An
// update without animate draws every 33 ms until closed.
namespace vat::ui_canvas {

constexpr std::size_t kMaxJson = 4096;
constexpr std::size_t kMaxOps = 64;
constexpr std::size_t kMaxText = 120;
constexpr int kMinCoord = -240;
constexpr int kMaxCoord = 480;

enum class Kind : uint8_t {
    FillScreen, DrawPixel, DrawLine, DrawFastHLine, DrawFastVLine, DrawRect, FillRect, DrawRoundRect,
    FillRoundRect, DrawCircle, FillCircle, DrawEllipse, FillEllipse, DrawTriangle, FillTriangle, DrawArc,
    FillArc, DrawBezier, DrawGradientLine, DrawString, DrawQr, DrawImage, ProgressBar, Rotate
};

constexpr uint32_t kMinFrameMs = 33;
constexpr std::size_t kMaxUpdate = 2048;  // bytes of Lua
constexpr uint32_t kMaxFrames = 600;

struct Op {
    Kind kind;
    std::vector<String> args;  // as written (numbers as text), "{n}" filled in by fill()
    String color = "white";
    String color2;             // DrawGradientLine end, ProgressBar background
    // DrawString
    int size = 1;              // 1..4 (text scale)
    String datum = "top_left";
};

struct Spec {
    std::vector<Op> ops;
    String source_tool;   // "" (no refresh), or the tool name (as ui_panel)
    String source_args;   // JSON
    uint32_t every_s = 0;
    int images = 0;       // drawImage ops (at most one)
    uint32_t frame_ms = 0;  // animation: 0 = still
    uint32_t frames = 0;    // 0 = until closed
    int step = 1;           // {t} = frame × step
    String vars_json;       // "vars": starting values ("" when none)
    String update;          // "update": Lua run before every frame ("" when none)
};

// false: `error` says which op or field is wrong.
bool parse(const String& json, Spec& out, String& error);
// Replaces "{n}" in every argument with line n of `lines` and "{n#}" with
// the first number in it ("" when missing).
void fill(std::vector<Op>& ops, const std::vector<String>& lines);
// Replaces "{t}" with `t` (an animation frame).
void fill_frame(std::vector<Op>& ops, long t);
// Replaces "{name}" with the value of `vars` (numbers as number_text).
using Lookup = std::function<bool(const String& name, String& value)>;
void fill_vars(std::vector<Op>& ops, const Lookup& value);
// Syntax check for "update" (the firmware sets lua_vm's; host tests run without).
using Compiler = String (*)(const String& source);
void set_compiler(Compiler compiler);
// Rotation by `degrees` about (cx, cy), applied to a point.
void rotate_point(int degrees, int cx, int cy, int& x, int& y);
// "#RRGGBB", "#RGB" or a colour name → RGB888; false when unknown.
bool color(const String& text, uint32_t& rgb);
// An argument as a number (after fill); false when it is not one.
bool number(const String& text, int& out);
const char* kind_name(Kind kind);

}  // namespace vat::ui_canvas
