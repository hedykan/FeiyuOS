#pragma once

#include <Arduino.h>
#include <cJSON.h>

// Air conditioners over the IR emitter: the IRext code database packed on the
// card (/vat/ir/ac.irdb, tools/pack_irdb.py), encoded by ac_core.  A brand has
// several remotes; until one is confirmed they are tried in the database's
// order, each waiting for the person's yes/no, like the TV codes in ops_ir.
// The profile (/vat/things/ir/<name>.json, "kind": "ac") keeps the brand, the
// confirmed remote and the state the remote holds, so "+1" or "关" works.
namespace vat::ac {

constexpr char kDatabase[] = "/vat/ir/ac.irdb";

bool is_ac(const String& name);
// `words`: the wanted state or the change ("奥克斯 开 制冷 26", "关", "+1",
// "除湿", "高风"); the brand is needed the first time.
String send(const String& name, const String& words);
// The person's answer to the last send: yes keeps the remote, no tries the next.
String confirm(const String& name, bool worked);
String summary(const cJSON* profile);  // one line for ir://list
String brands_text();                  // brands in the database, most common first

}  // namespace vat::ac
