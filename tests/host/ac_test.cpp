// Host tests for air conditioners (ac_core.cpp): the command words, the packed
// code database (assets/irdb/ac.irdb from tools/pack_irdb.py) and encoding with
// the IRext decoder.  Reference pulse trains for AUX remote 9377 came from the
// decoder's own test harness run against the original IRext code file.
// Included by host_test.cpp; uses its CHECK macro.
#include <fstream>
#include <iterator>
#include <numeric>

#include "ac_core.h"

namespace {

using vat::ac::State;

State applied(const char* words, State state = State()) {
    String brand, error;
    vat::ac::apply(words, state, brand, error);
    return state;
}

void ac_words() {
    String brand, error;
    State s;
    CHECK(vat::ac::apply("奥克斯 开 制冷 24度 高风", s, brand, error));
    CHECK(brand == "奥克斯");
    CHECK(s.on && s.mode == vat::ac::kCool && s.temp == 24 && s.fan == vat::ac::kFanHigh);
    CHECK(vat::ac::describe(s) == "开，制冷 24°C，高风");

    CHECK(!applied("关").on);
    State off;
    off.on = false;
    CHECK(applied("26", off).on);                 // a setting alone turns it on
    CHECK(!applied("关 26", off).on);             // unless power is said too
    CHECK(applied("+2").temp == 28);
    CHECK(applied("调低").temp == 25);
    CHECK(applied("+9").temp == 30);               // clamped
    CHECK(applied("mode=auto").mode == vat::ac::kAuto);
    CHECK(applied("fan=auto", applied("高风")).fan == vat::ac::kFanAuto);
    CHECK(applied("除湿").mode == vat::ac::kDry);
    CHECK(applied("temp=22°C").temp == 22);

    State same;
    brand = "";
    CHECK(!vat::ac::apply("auto", same, brand, error));  // mode or fan?
    CHECK(!vat::ac::apply("35度", same, brand, error));
    brand = "";
    CHECK(!vat::ac::apply("格力 美的", same, brand, error));
    CHECK(same.temp == 26);  // a failed apply changes nothing

    State before, after;
    after.temp = 27;
    CHECK(vat::ac::key_for(before, after) == 2);   // KEY_AC_TEMP_PLUS
    after = before;
    after.on = false;
    CHECK(vat::ac::key_for(before, after) == 0);   // KEY_AC_POWER

    cJSON* object = cJSON_CreateObject();
    vat::ac::to_json(applied("关 制热 20 低风"), object);
    const State back = vat::ac::from_json(object);
    CHECK(!back.on && back.mode == vat::ac::kHeat && back.temp == 20 && back.fan == vat::ac::kFanLow);
    cJSON_Delete(object);
}

void ac_index_and_encode() {
    std::ifstream in("assets/irdb/ac.irdb", std::ios::binary);
    const std::vector<uint8_t> db((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(db.size() > 100000);
    if (db.size() < 100) return;
    const std::string all(db.begin(), db.end());
    const std::size_t header_end = all.find('\n');
    const long index_len = vat::ac::index_length(String(all.substr(0, header_end)));
    CHECK(index_len > 0);
    const std::size_t blob = header_end + 1 + static_cast<std::size_t>(index_len);
    CHECK(blob < db.size());

    // Walk the index the way the device does: line by line, first match wins.
    vat::ac::Brand aux;
    int brands = 0;
    bool found = false;
    std::size_t at = header_end + 1;
    while (at < blob) {
        const std::size_t end = all.find('\n', at);
        vat::ac::Brand b;
        CHECK(vat::ac::parse_brand(String(all.substr(at, end - at)), b));
        ++brands;
        if (!found && vat::ac::brand_matches(b, "奥克斯空调")) {
            aux = b;
            found = true;
        }
        at = end + 1;
    }
    CHECK(brands > 200);
    CHECK(found && aux.name == "奥克斯" && aux.remotes.size() == 13);
    CHECK(aux.remotes.size() > 1 && aux.remotes[0].id == "9377" && aux.remotes[1].id == "11832");
    vat::ac::Brand gree;
    CHECK(vat::ac::parse_brand("格力|GREE|1@0+5", gree) && vat::ac::brand_matches(gree, "gree"));
    CHECK(!vat::ac::brand_matches(gree, "美的"));

    if (aux.remotes.empty()) return;
    const vat::ac::Remote& r = aux.remotes[0];
    const std::vector<uint8_t> code(db.begin() + blob + r.offset, db.begin() + blob + r.offset + r.length);
    const auto total = [](const std::vector<uint16_t>& v) { return std::accumulate(v.begin(), v.end(), 0L); };
    std::vector<uint16_t> pulses;
    String error;

    State s = applied("开 制冷 26 自动风");
    CHECK(vat::ac::encode(code, s, 0, pulses, error));  // KEY_AC_POWER
    CHECK(pulses.size() == 211 && total(pulses) == 147885);
    CHECK(pulses.size() > 3 && pulses[0] == 8979 && pulses[1] == 4525 && pulses[2] == 550 && pulses[3] == 1693);

    State cooler = applied("24", s);
    CHECK(vat::ac::encode(code, cooler, vat::ac::key_for(s, cooler), pulses, error));
    CHECK(pulses.size() == 211 && total(pulses) == 144456);

    State off = applied("关", cooler);
    CHECK(vat::ac::encode(code, off, vat::ac::key_for(cooler, off), pulses, error));
    CHECK(pulses.size() == 211 && total(pulses) == 146742);

    CHECK(!vat::ac::encode({1, 2, 3}, s, 0, pulses, error));  // not a code file
}

void ac_core() {
    ac_words();
    ac_index_and_encode();
}

}  // namespace
