// console::Buffer: lines, "\r" progress lines, the byte limit, error lines.
#include "console_buffer.h"

void console_buffer() {
    using vat::console::Buffer;
    Buffer b(64);
    b.write("hello\nworld");
    CHECK(b.lines().size() == 2 && b.lines()[0].text == "hello" && b.lines()[1].text == "world");
    b.write("!\n\n");
    CHECK(b.lines().size() == 3 && b.lines()[1].text == "world!" && b.lines()[2].text == "");
    b.clear();
    b.write("进度 10%\r进度 50%\r进度 100%\n完成\n");
    CHECK(b.lines().size() == 2 && b.lines()[0].text == "进度 100%" && b.lines()[1].text == "完成");
    CHECK(b.bytes() == String("进度 100%").length() + String("完成").length());
    b.clear();
    b.write("ok\n");
    b.write("bad\n", true);
    CHECK(!b.lines()[0].error && b.lines()[1].error);
    b.clear();
    for (int i = 0; i < 20; ++i) b.write("line " + String(i) + "\n");
    CHECK(b.bytes() <= 64 && b.lines().back().text == "line 19" && b.lines().front().text != "line 0");
    Buffer one(8);
    one.write("0123456789abcdef");  // one line longer than the limit stays (the newest is never dropped)
    CHECK(one.lines().size() == 1 && one.lines()[0].text.length() == 16);
}
