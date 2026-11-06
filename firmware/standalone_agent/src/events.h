#pragma once

#include <Arduino.h>

// Event bus: a package that notices something (a timer is due, the device
// booted) emits it as a target string; whoever subscribed gets it.  The bus
// knows no package on either side.  Handlers run on the emitting task and
// must only queue work.
namespace vat::events {

// `data` is a JSON object with the event's fields ("{}" when it has none).
using Handler = void (*)(const String& target, const String& data);

void subscribe(Handler handler);  // during setup (registration), before the first emit
void emit(const String& target, const String& data = "{}");

}  // namespace vat::events
