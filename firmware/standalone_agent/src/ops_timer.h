#pragma once

#include <Arduino.h>

// Reminders: "in 10m" or "at 18:30".  Kept in /vat/things/timers/timers.json
// (wall-clock ones survive a reboot; before NTP sync only relative ones work
// and they live in RAM).  When one is due the UI task shows it full screen,
// beeps three times and leaves a notice on the Agent page.
namespace vat::timer {

void begin();  // after storage and time zone setup
void loop();   // UI task, cheap: checks once a second

// `kind` is "in" (10m, 90s, 1h30m, 2小时) or "at" (18:30, 2026-11-06 08:00).
String add(const String& kind, const String& when, const String& text);
String list_text();
String cancel(const String& which);  // number from the list, "all", or text

// Time events for the event bus.  time://at/18:30 | time://at/2026-11-08 09:00
// | time://in/10m (becomes an absolute at) | time://every/07:00 |
// time://weekday/09:00 | time://weekend/10:00; an optional "#tag" suffix is
// kept as part of the identity.  When due, the target is emitted (at: once).
// RAM only: whoever watches registers again after a reboot.  "" when watched.
String watch(const String& target, String& normalized);
void unwatch(const String& target);

}  // namespace vat::timer
