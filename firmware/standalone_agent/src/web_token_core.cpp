#include "web_token_core.h"

namespace vat::web_token {

String encode(const uint8_t* bytes, std::size_t n) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    String out;
    uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        buffer = (buffer << 8) | bytes[i];
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 31];
    return out;
}

bool same(const String& a, const String& b) {
    if (a.isEmpty() || a.length() != b.length()) return false;
    uint8_t diff = 0;
    for (std::size_t i = 0; i < a.length(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

String presented(const String& authorization, const String& cookie, const String& arg) {
    if (authorization.startsWith("Bearer ")) {
        String t = authorization.substring(7);
        t.trim();
        if (!t.isEmpty()) return t;
    }
    const int at = cookie.indexOf("vat_token=");
    if (at >= 0 && (at == 0 || cookie[at - 1] == ' ' || cookie[at - 1] == ';')) {
        int end = cookie.indexOf(';', at);
        if (end < 0) end = static_cast<int>(cookie.length());
        String t = cookie.substring(at + 10, end);
        t.trim();
        if (!t.isEmpty()) return t;
    }
    return arg;
}

}  // namespace vat::web_token
