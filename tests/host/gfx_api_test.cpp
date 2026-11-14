// Host tests for the gfx function table (gfx_api.cpp): every row's example
// checks out, overloads and optional arguments, the argument types, the byte
// limit, and gfx://help names every function.  Included by host_test.cpp.
#include "gfx_api.h"

namespace {

// A row's Lua example as a JSON array: 'text' -> "text", {…} -> […].
cJSON* example_args(const char* example) {
    String json = "[";
    for (const char* c = example; *c; ++c) json += *c == '\'' ? '"' : *c == '{' ? '[' : *c == '}' ? ']' : *c;
    json += "]";
    return cJSON_Parse(json.c_str());
}

bool accepts(const char* name, const char* json, int* overload_out = nullptr, String* first = nullptr) {
    const auto* fn = vat::gfx_api::find(name);
    cJSON* args = cJSON_Parse(json);
    std::vector<String> out;
    int overload = -1;
    std::size_t bytes = 0;
    String error;
    const bool ok = fn && args && vat::gfx_api::check(*fn, args, out, overload, bytes, error);
    cJSON_Delete(args);
    if (overload_out) *overload_out = overload;
    if (first && !out.empty()) *first = out.back();
    return ok;
}

void gfx_api() {
    using namespace vat::gfx_api;
    // Every row: its example checks out (rows without one take no arguments or need a file).
    int with_examples = 0;
    for (std::size_t i = 0; i < count(); ++i) {
        const Fn& fn = table()[i];
        CHECK(find(fn.name) == &fn && index_of(&fn) == static_cast<int>(i));
        if (!*fn.example) continue;
        cJSON* args = example_args(fn.example);
        std::vector<String> out;
        int overload = -1;
        std::size_t bytes = 0;
        String error;
        const bool ok = args && check(fn, args, out, overload, bytes, error);
        if (!ok) printf("  example of %s: %s\n", fn.name, error.c_str());
        CHECK(ok);
        cJSON_Delete(args);
        ++with_examples;
    }
    CHECK(count() >= 75 && with_examples >= 60);

    // Optional colour, wrong counts, overloads.
    CHECK(accepts("fillRect", "[1,2,3,4]") && accepts("fillRect", "[1,2,3,4,\"red\"]"));
    CHECK(!accepts("fillRect", "[1,2,3]") && !accepts("fillRect", "[1,2,3,4,\"red\",5]"));
    int overload = -1;
    CHECK(accepts("drawBezier", "[1,2,3,4,5,6,7,8,\"red\"]", &overload) && overload == 1);
    CHECK(!accepts("drawBezier", "[1,2,3,4,5,6,7,8]"));  // M5GFX: the 4-point curve needs its colour
    CHECK(accepts("setColor", "[255,0,0]", &overload) && overload == 1);
    CHECK(accepts("drawChar", "[10,10,\"A\",\"red\",\"black\",2]", &overload) && overload == 1);

    // Argument types.
    String last;
    CHECK(accepts("fillScreen", "[63488]", nullptr, &last) && last == "#FF0000");  // RGB565 red, as M5GFX
    CHECK(accepts("fillScreen", "[\"#3FB8AF\"]", nullptr, &last) && last == "#3FB8AF");
    CHECK(!accepts("fillScreen", "[\"nocolor\"]"));
    CHECK(accepts("setFont", "[4]", nullptr, &last) && last == "Font4");
    CHECK(accepts("setFont", "[\"CN14\"]", nullptr, &last) && last == "cn14");
    CHECK(!accepts("setFont", "[\"FreeSans9pt7b\"]"));
    CHECK(accepts("setTextDatum", "[\"middle_center\"]", nullptr, &last) && last == "5");
    CHECK(!accepts("setTextDatum", "[\"centre\"]"));
    CHECK(accepts("drawChar", "[\"中\",1,1]") && accepts("fillGradientRect", "[0,0,10,10,\"red\",\"blue\",\"VLINEAR\"]"));
    CHECK(accepts("fillRect", "[\"{x}\",2,3,4,\"{c}\"]"));  // placeholders, filled before drawing
    CHECK(accepts("drawBitmap", "[0,0,\"183C7EFF\",8,4,\"white\"]", nullptr, &last));
    CHECK(!accepts("drawBitmap", "[0,0,[256],8,1,\"white\"]"));
    CHECK(!accepts("drawPng", "[\"/etc/passwd\",0,0]") && accepts("drawPng", "[\"/vat/a.png\"]"));
    CHECK(!accepts("pushImage", "[0,0,1,1,[70000]]"));
    CHECK(!accepts("fillRect", "[1e9,0,1,1]"));  // out of range

    // The byte limit counts across the calls of a frame.
    {
        const Fn* fn = find("drawBitmap");
        String big = "[0,0,\"";
        for (int i = 0; i < 20000; ++i) big += "FF";
        big += "\",8,8,\"white\"]";
        cJSON* args = cJSON_Parse(big.c_str());
        std::vector<String> out;
        int k = 0;
        std::size_t bytes = 0;
        String error;
        CHECK(check(*fn, args, out, k, bytes, error) && bytes == 20000);
        CHECK(!check(*fn, args, out, k, bytes, error) && error.indexOf("32 KB") >= 0);
        cJSON_Delete(args);
    }

    // Queries: not in canvases.
    vat::ui_canvas::Spec spec;
    String error;
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["textWidth","abc"]]})", spec, error) && error.indexOf("查询") >= 0);
    CHECK(vat::ui_canvas::parse(R"({"ops":[["setTextColor","red"],["print","hi"],["fillSmoothCircle",10,10,5]]})", spec, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["blink",1]]})", spec, error) && error.indexOf("gfx://help") >= 0);

    // gfx://help names every function.
    const String help_text = help();
    for (std::size_t i = 0; i < count(); ++i) CHECK(help_text.indexOf(String(table()[i].name) + "(") >= 0);
}

}  // namespace
