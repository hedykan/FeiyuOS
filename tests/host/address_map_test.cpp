// Host tests for address::rewrite (address_map.cpp): the same cases as
// tools/readdress.py, so the card migration and the repository tool agree.
#include "address_map.h"

namespace {

void address_map() {
    using vat::address::rewrite;
    CHECK(rewrite("write lora://settings/enabled") == "write settings://lora/enabled");
    CHECK(rewrite("read lora://status") == "read device://local/lora/status");
    CHECK(rewrite("ble://power") == "settings://ble/power");
    CHECK(rewrite("read ble://devices") == "read device://local/ble/devices/list");
    CHECK(rewrite("ble://devices/x") == "device://local/ble/devices/x");
    CHECK(rewrite("write ble://scan/log") == "write device://local/ble/log");
    CHECK(rewrite("ir://客厅电视/power") == "device://local/ir/客厅电视/power");
    CHECK(rewrite("pkg://install/天气") == "pkg://天气/install");
    CHECK(rewrite("pkg://update/测试-安装 更新") == "pkg://测试-安装/update 更新");
    CHECK(rewrite("pkg://install?url=x") == "pkg://new?url=x");
    CHECK(rewrite("settings://open/key/qwen") == "settings://key/qwen/open");
    CHECK(rewrite("settings://open/wifi") == "settings://wifi/open");
    CHECK(rewrite("read ui://apps") == "read ui://apps/list");
    CHECK(rewrite("ui://apps/名字/open") == "ui://apps/名字/open");
    CHECK(rewrite("memory://cases") == "memory://cases/list");
    CHECK(rewrite("memory://cases/x") == "memory://cases/x");
    CHECK(rewrite("memory://out") == "memory://out/list");
    CHECK(rewrite("memory://out/3") == "memory://out/3");
    CHECK(rewrite("device://boot") == "device://local/event/boot");
    CHECK(rewrite("device://local/display/brightness") == "settings://brightness");
    CHECK(rewrite("\"wifi://家里 WiFi\"") == "\"device://local/wifi/家里 WiFi\"");
    CHECK(rewrite("timer://in/10m") == "timer://in/10m");
    // In a program: @uses lines, calls, JSON
    CHECK(rewrite("-- @uses write ir://空调/*\nvat.write(\"ir://空调/ac\", \"开\")") ==
          "-- @uses write device://local/ir/空调/*\nvat.write(\"device://local/ir/空调/ac\", \"开\")");
    CHECK(rewrite("{\"when\":\"device://boot\",\"run\":\"prog://x\"}") ==
          "{\"when\":\"device://local/event/boot\",\"run\":\"prog://x\"}");
    CHECK(rewrite("vat.read(\"device://local/ir/list\")") == "vat.read(\"device://local/ir/list\")");  // already current
    CHECK(rewrite("pkg://install/。") == "pkg://install/。");  // no name: left as is
    // A call to a retired target names the current one; current targets are not retired
    CHECK(vat::address::retired("ble://devices") == "device://local/ble/devices/list");
    CHECK(vat::address::retired("pkg://install/天气") == "pkg://天气/install");
    CHECK(vat::address::retired("device://local/ble/devices/list").isEmpty());
    CHECK(vat::address::retired("pkg://天气/install").isEmpty());
    CHECK(vat::address::retired("").isEmpty());
    // 0.7.5: canvases moved to gfx
    CHECK(rewrite("vat.write(\"ui://canvas/钟\", c)") == "vat.write(\"gfx://canvas/钟\", c)");
    CHECK(vat::address::retired("ui://canvas/钟/close") == "gfx://canvas/钟/close");
    CHECK(vat::address::retired("ui://panel/钟").isEmpty());
}

}  // namespace
