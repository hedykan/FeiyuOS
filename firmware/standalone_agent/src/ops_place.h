#pragma once

#include <Arduino.h>

// AMap (高德) location services for the place package.  Read-only web calls to
// restapi.amap.com (reachable directly in China, no proxy); the key is
// /vat/secrets.json providers.amap.api_key.  The current position comes from
// the GPS fix (converted WGS-84 -> GCJ-02 by AMap), falling back to IP
// location when there is no fix.  Replies are short text; the agent summarizes.
namespace vat::place {

bool available();  // the AMap key is set

String here();                                       // reverse geocode the current position
String nearby(const String& keyword);                // POIs around the current position
String search(const String& keyword, const String& city);  // keyword search
String weather();                                    // weather for the current city
String route(const String& destination, const String& mode);  // walk|drive|bike|ebike|transit (v5), text steps

}  // namespace vat::place
