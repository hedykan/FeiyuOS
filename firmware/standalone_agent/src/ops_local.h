#pragma once

#include <Arduino.h>
#include <cJSON.h>

namespace vat::local {

// Resources owned by the device itself:
//   device://local/{status,settings,battery,display/brightness,display/timeout,
//                   display/power,audio/volume,clipboard}
//   memory://{recent-tasks,notes}
bool owns(const String& target);
String tool_read(const cJSON* arguments);
String tool_write(const cJSON* arguments);
String resources_json();
void probe_hardware();  // once at boot, after M5Cardputer.begin()
String status_json(bool for_model = false);  // for_model: without the IP and BSSID
// Main loop: idle share of each core over the last 10 s window (FreeRTOS run-time stats).
void sample_cpu();
String clipboard();

}  // namespace vat::local
