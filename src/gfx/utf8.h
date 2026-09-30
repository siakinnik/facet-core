#pragma once

#include <cstdint>
#include <string_view>

namespace facet::gfx {

// Decodes one code point starting at s[i] and advances i. Invalid bytes
// yield U+FFFD.
inline uint32_t utf8_next(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size()) return 0xFFFD;
        unsigned char cc = static_cast<unsigned char>(s[i]);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        ++i;
    }
    return cp;
}

}  // namespace facet::gfx
