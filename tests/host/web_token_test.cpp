// Host tests for the web API token (web_token_core.cpp).  Included by
// host_test.cpp; uses its CHECK macro.
#include "web_token_core.h"

namespace {

void web_token() {
    const uint8_t f[] = {'f', 'o', 'o', 'b', 'a', 'r'};
    CHECK(vat::web_token::encode(f, 6) == "MZXW6YTBOI");  // RFC 4648 test vector, no padding
    uint8_t random[vat::web_token::kBytes] = {};
    for (std::size_t i = 0; i < sizeof random; ++i) random[i] = static_cast<uint8_t>(i * 37 + 11);
    const String token = vat::web_token::encode(random, sizeof random);
    CHECK(token.length() == 26);

    CHECK(vat::web_token::same(token, String(token.c_str())));
    CHECK(!vat::web_token::same(token, token.substring(0, 25) + "A") || token[25] == 'A');
    CHECK(!vat::web_token::same(token, token.substring(0, 25)));
    CHECK(!vat::web_token::same("", ""));  // no token is never a match

    CHECK(vat::web_token::presented("Bearer ABC", "", "") == "ABC");
    CHECK(vat::web_token::presented("", "theme=dark; vat_token=XYZ; x=1", "") == "XYZ");
    CHECK(vat::web_token::presented("", "vat_token=XYZ", "") == "XYZ");
    CHECK(vat::web_token::presented("", "not_vat_token=XYZ", "Q") == "Q");  // only the exact cookie name
    CHECK(vat::web_token::presented("Basic abc", "", "ARG") == "ARG");
    CHECK(vat::web_token::presented("", "", "") == "");
}

}  // namespace
