#pragma once

#include <Arduino.h>

// Downloads for pkg://: the online catalog, installing and updating from it,
// installing from an address, rolling back.  Files are checked before they
// reach /vat/things/lua: SHA-256 and size against the catalog, the header
// parsed, the code compiled; from the catalog the header must declare exactly
// the uses / target / usage the catalog showed when the user confirmed.
namespace vat::pkg_store {

String catalog(const String& url);  // read pkg://catalog[?url=…]: fetch, keep, list
// write pkg://install/名字 · pkg://update/名字 (from the catalog last read)
String install(const String& name, bool update);
// write pkg://install?[sha256=…&]url=… (url last): ends up waiting for enable
String install_url(const String& url, const String& sha256);
// The confirmation text for install / update / install?url / rollback.
String summary(const String& verb, const String& name, const String& url, const String& sha256);

}  // namespace vat::pkg_store
