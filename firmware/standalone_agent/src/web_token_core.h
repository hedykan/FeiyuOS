#pragma once

#include <cstddef>
#include <cstdint>

#include "common.h"

// The web API token: random bytes shown as Base32 (no 0/1/8/9, easy to read
// off the screen), and how a request presents it.  No device calls, so it
// builds on the host; web_token.cpp adds the random source and storage.
namespace vat::web_token {

constexpr std::size_t kBytes = 16;  // 26 Base32 characters

String encode(const uint8_t* bytes, std::size_t n);
// Constant time for equal lengths: a wrong token takes as long as a near miss.
bool same(const String& a, const String& b);
// The token a request carries: "Authorization: Bearer <t>", then the
// "vat_token" cookie, then the token= argument.  "" when none.
String presented(const String& authorization, const String& cookie, const String& arg);

}  // namespace vat::web_token
