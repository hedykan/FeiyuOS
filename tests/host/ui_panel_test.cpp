// Host tests for panel descriptions (ui_panel.cpp).  Included by
// host_test.cpp; uses its CHECK macro.
#include "ui_panel.h"

namespace {

void ui_panel() {
    using vat::ui_panel::Spec;
    Spec s;
    String error;
    CHECK(vat::ui_panel::parse(R"({"title":"负载","lines":["a","b"],"choices":["重启","日志"],)"
                               R"("source":{"exec":"ssh://开发机/uptime","every":"30s"}})", s, error));
    CHECK(s.title == "负载" && s.lines.size() == 2 && s.choices.size() == 2);
    CHECK(s.source_tool == "exec" && s.source_args == R"({"target":"ssh://开发机/uptime"})" && s.every_s == 30);

    // Round trip keeps everything.
    Spec back;
    CHECK(vat::ui_panel::parse(vat::ui_panel::to_json(s), back, error));
    CHECK(back.title == s.title && back.lines == s.lines && back.choices == s.choices && back.every_s == 30);
    CHECK(back.source_tool == "exec" && back.source_args == s.source_args);
    CHECK(vat::ui_panel::parse(R"({"source":{"tool":"exec","arguments":{"target":"ssh://开发机","program":"uptime"}}})", s, error));
    CHECK(s.source_tool == "exec" && s.source_args.indexOf("program") >= 0 && s.every_s == 0);
    CHECK(!vat::ui_panel::parse(R"({"source":{"tool":"exec"}})", s, error));  // tool needs arguments

    CHECK(vat::ui_panel::parse(R"({"source":{"read":"sensor://gps/location","every":"5m"}})", s, error) && s.every_s == 300);
    CHECK(vat::ui_panel::parse(R"({"choices":["是","否"]})", s, error) && s.source_tool.isEmpty());

    CHECK(!vat::ui_panel::parse("[1]", s, error));
    CHECK(!vat::ui_panel::parse(R"({"title":"x","script":"rm"})", s, error) && error.indexOf("script") >= 0);
    CHECK(!vat::ui_panel::parse(R"({"lines":"不是数组"})", s, error));
    CHECK(!vat::ui_panel::parse(R"({"choices":[1,2]})", s, error));
    CHECK(!vat::ui_panel::parse(R"({"choices":["1","2","3","4","5","6","7","8","9"]})", s, error));
    CHECK(!vat::ui_panel::parse(R"({"source":{"read":"a","exec":"b"}})", s, error));
    CHECK(!vat::ui_panel::parse(R"({"source":{"read":"a","every":"5s"}})", s, error));  // under 10 s
    CHECK(!vat::ui_panel::parse(R"({"source":{"read":"a","every":"soon"}})", s, error));
    CHECK(!vat::ui_panel::parse("{}", s, error));
    CHECK(!vat::ui_panel::parse(String(R"({"title":")") + String(std::string(3000, 'x')) + "\"}", s, error));

    const auto lines = vat::ui_panel::lines_from(String(" up 3 days \n\n load 0.1\n") + String(std::string(200, 'y')));
    CHECK(lines.size() == 3 && lines[0] == "up 3 days" && lines[1] == "load 0.1" && lines[2].length() == 120);
    std::string many;
    for (int i = 0; i < 20; ++i) many += "l\n";
    CHECK(vat::ui_panel::lines_from(String(many)).size() == 12);

    CHECK(vat::ui_panel::valid_name("开发机负载"));
    CHECK(!vat::ui_panel::valid_name("") && !vat::ui_panel::valid_name("a/b") && !vat::ui_panel::valid_name("a?b"));
}

}  // namespace
