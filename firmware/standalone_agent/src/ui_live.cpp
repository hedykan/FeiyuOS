#include "ui_live.h"

#include <mutex>

#include "common.h"
#include "gfx_exec.h"
#include "policy.h"
#include "registry.h"
#include "storage.h"
#include "ui_state.h"
#include "worker.h"

namespace vat::ui_live {
namespace {

using ui::open_panel;
using ui::Overlay;
using ui::post_overlay;

// The panel or canvas on screen with a source: refreshed from the worker while open.
std::mutex lock;
String live_name;
ui_panel::Spec live;
unsigned long next_refresh = 0;
bool refresh_queued = false;
bool live_seen = false;  // the live panel has been on screen
bool live_is_canvas = false;
ui_canvas::Spec live_canvas;

// A canvas with its source lines filled in; the drawImage file is read here
// (worker) so the UI task only draws.
void post_canvas(const String& name, const ui_canvas::Spec& spec, const std::vector<String>& lines) {
    Overlay overlay;
    overlay.kind = Overlay::Kind::Canvas;
    overlay.panel = name;
    overlay.canvas = spec.ops;
    overlay.frame_ms = spec.frame_ms;
    overlay.frames = spec.frames;
    overlay.step = spec.step;
    overlay.vars_json = spec.vars_json;
    overlay.update = spec.update;
    ui_canvas::fill(overlay.canvas, lines);
    String error;
    if (!gfx_exec::prepare(overlay.canvas, overlay.blobs, error)) runtime_event("canvas_bytes", error);
    for (const auto& op : overlay.canvas) {
        if (op.kind != ui_canvas::Kind::DrawImage) continue;
        ui_canvas::number(op.args[1], overlay.image_x);
        ui_canvas::number(op.args[2], overlay.image_y);
        if (!storage::read_bytes(op.args[0], overlay.image, 32768)) overlay.image.clear();
    }
    post_overlay(std::move(overlay));
}

void post_panel(const String& name, const ui_panel::Spec& spec) {
    Overlay overlay;
    overlay.kind = Overlay::Kind::Panel;
    overlay.panel = name;
    overlay.title = spec.title.isEmpty() ? name : spec.title;
    overlay.lines = spec.lines;
    overlay.choices = spec.choices;
    post_overlay(std::move(overlay));
}

// Worker: run the live source and show the result.
void refresh_job(uint32_t id, const String& name) {
    ui_panel::Spec spec;
    bool canvas;
    ui_canvas::Spec canvas_spec;
    {
        std::lock_guard<std::mutex> guard(lock);
        refresh_queued = false;
        if (live_name != name) return;
        spec = live;
        canvas = live_is_canvas;
        canvas_spec = live_canvas;
        if (canvas) {
            spec.source_tool = canvas_spec.source_tool;
            spec.source_args = canvas_spec.source_args;
        }
    }
    if (open_panel() != name) return;
    cJSON* args = cJSON_Parse(spec.source_args.c_str());
    String text;
    if (!args) {
        text = "刷新来源格式不对";
    } else {
        if (!cJSON_GetObjectItemCaseSensitive(args, "confirm_required")) cJSON_AddBoolToObject(args, "confirm_required", false);
        const auto verdict = policy::evaluate(spec.source_tool, args);
        text = verdict.decision == policy::Decision::Allow ? registry::execute(spec.source_tool, args, false)
                                                         : "这个来源需要确认，不能自动刷新（" + verdict.reason + "）";
        cJSON_Delete(args);
    }
    worker::set_result(id, "refreshed " + name);
    if (open_panel() != name) return;
    if (canvas) return post_canvas(name, canvas_spec, ui_panel::lines_from(text));
    spec.lines = ui_panel::lines_from(text);
    post_panel(name, spec);
}

}  // namespace

void show_canvas(const String& name, const ui_canvas::Spec& spec) {
    post_canvas(name, spec, {});
    std::lock_guard<std::mutex> guard(lock);
    live_name = spec.source_tool.isEmpty() ? String() : name;
    live_is_canvas = true;
    live_canvas = spec;
    live_seen = false;
    next_refresh = millis();
}

void show_panel(const String& name, const ui_panel::Spec& spec) {
    post_panel(name, spec);
    std::lock_guard<std::mutex> guard(lock);
    live_name = spec.source_tool.isEmpty() ? String() : name;
    live = spec;
    live_is_canvas = false;
    live_seen = false;
    next_refresh = millis();  // the first refresh right away
}

void loop() {
    String name;
    uint32_t every_s;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (live_name.isEmpty() || refresh_queued || static_cast<long>(millis() - next_refresh) < 0) return;
        name = live_name;
        every_s = live_is_canvas ? live_canvas.every_s : live.every_s;
    }
    const bool on_screen = open_panel() == name;
    if (!on_screen) {
        // Not drawn yet (just posted): wait.  Seen and gone: closed or replaced, stop.
        std::lock_guard<std::mutex> guard(lock);
        if (live_seen && live_name == name) live_name = "";
        return;
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        live_seen = true;
    }
    if (worker::busy() || !worker::submit({worker::Kind::Callback, name, "", {}, false, refresh_job})) return;
    std::lock_guard<std::mutex> guard(lock);
    refresh_queued = true;
    next_refresh = millis() + (every_s ? every_s * 1000UL : 0x7FFFFFFFUL);
}

}  // namespace vat::ui_live
