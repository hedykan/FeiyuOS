// Host tests for canvases (ui_canvas.cpp).  Included by host_test.cpp; uses
// its CHECK macro.
#include "ui_canvas.h"

namespace {

void ui_canvas() {
    using vat::ui_canvas::Kind;
    vat::ui_canvas::Spec s;
    String error;
    CHECK(vat::ui_canvas::parse(R"({"ops":[["fillScreen","#101820"],["fillRoundRect",10,10,100,40,6,"cyan"],)"
                                R"(["drawArc",180,70,30,24,0,270,"#E8A33D"],)"
                                R"(["drawString","{1}",120,60,{"size":3,"color":"white","datum":"middle_center"}],)"
                                R"(["progressBar",10,110,220,10,"{2}","#3FB8AF","gray"],)"
                                R"(["drawGradientLine",0,0,239,0,"red","blue"]],)"
                                R"("source":{"read":"device://local/battery","every":"30s"}})", s, error));
    CHECK(s.ops.size() == 6 && s.ops[1].kind == Kind::FillRoundRect && s.ops[1].args.size() == 5);
    CHECK(s.ops[3].kind == Kind::DrawString && s.ops[3].size == 3 && s.ops[3].datum == "middle_center");
    CHECK(s.ops[4].color2 == "gray" && s.source_tool == "read" && s.every_s == 30);

    s.ops[3].args[0] = "{2#}%";
    vat::ui_canvas::fill(s.ops, {"26°C", "电量 87%，电压 4.2 V"});
    CHECK(s.ops[3].args[0] == "87%");
    int v = 0;
    CHECK(vat::ui_canvas::number(s.ops[4].args[4], v) && v == 87);  // first number of the line
    CHECK(!vat::ui_canvas::number("没有数字", v));

    uint32_t rgb = 0;
    CHECK(vat::ui_canvas::color("#3FB8AF", rgb) && rgb == 0x3FB8AF);
    CHECK(vat::ui_canvas::color("#fff", rgb) && rgb == 0xFFFFFF);
    CHECK(vat::ui_canvas::color("Cyan", rgb) && rgb == 0x3FB8AF);
    CHECK(!vat::ui_canvas::color("#12345", rgb) && !vat::ui_canvas::color("chartreuse", rgb));

    CHECK(vat::ui_canvas::parse(R"({"ops":[["drawQr","https://x",0,0,3],["drawImage","/vat/assets/a.png",0,0]]})", s, error));
    CHECK(s.images == 1);

    CHECK(!vat::ui_canvas::parse(R"({"ops":[["system","rm -rf"]]})", s, error) && error.indexOf("不支持") >= 0);
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillRect",1,2,3,"red"]]})", s, error));          // a number short
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillRect",1,2,3,4,"plaid"]]})", s, error));      // unknown colour
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillRect",1,2,3,99999,"red"]]})", s, error));    // out of range
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["drawImage","/etc/passwd",0,0]]})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["drawImage","/vat/a.png",0,0],["drawImage","/vat/b.png",0,0]]})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["drawString","x",0,0,{"size":9}]]})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["drawGradientLine",0,0,1,1,"red"]]})", s, error));  // needs two colours
    CHECK(!vat::ui_canvas::parse(R"({"ops":[]})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillScreen","red"]],"script":1})", s, error));
    // Rotation and animation.
    CHECK(vat::ui_canvas::parse(R"({"ops":[["rotate","{t}",120,67],["fillEllipse",120,67,50,20,"cyan"],["rotate",0,0,0]],)"
                                R"("animate":{"every":"100ms","frames":36,"step":10}})", s, error));
    CHECK(s.ops.size() == 3 && s.ops[0].kind == Kind::Rotate && s.frame_ms == 100 && s.frames == 36 && s.step == 10);
    vat::ui_canvas::fill_frame(s.ops, 90);
    CHECK(s.ops[0].args[0] == "90");
    int x = 170, y = 67;
    vat::ui_canvas::rotate_point(90, 120, 67, x, y);
    CHECK(x == 120 && y == 117);  // screen y grows downwards: +90° turns right into down
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillScreen","red"]],"animate":{"every":"10ms"}})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["fillScreen","red"]],"animate":{"every":"1s","frames":9999}})", s, error));
    CHECK(!vat::ui_canvas::parse(R"({"ops":[["rotate",10,0]]})", s, error));  // needs cx, cy
    std::string many = R"({"ops":[)";
    for (int i = 0; i < 65; ++i) many += std::string(i ? "," : "") + R"(["drawPixel",1,1,"red"])";
    many += "]}";
    CHECK(!vat::ui_canvas::parse(String(many), s, error));
}

}  // namespace
