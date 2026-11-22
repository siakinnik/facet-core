// Writing frames into framebuffer memory: only the part of each row that
// changed since the page was last written (framebuffer memory is slow to
// write and slower to read, so a shadow copy says what it holds), converted
// to the framebuffer's pixel layout on the way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace facet::platform {

struct FbLayout {
    enum class Format { Xrgb, Xbgr, Rgb565, Other } format = Format::Other;
    int bpp = 32;
    uint32_t alpha = 0;  // alpha bits set to opaque, 0 without an alpha channel
    uint32_t r_off = 16, r_len = 8, g_off = 8, g_len = 8, b_off = 0, b_len = 8;  // Format::Other

    static FbLayout make(int bpp, uint32_t r_off, uint32_t r_len, uint32_t g_off, uint32_t g_len, uint32_t b_off,
                         uint32_t b_len, uint32_t a_off, uint32_t a_len) {
        FbLayout L;
        L.bpp = bpp;
        L.r_off = r_off, L.r_len = r_len, L.g_off = g_off, L.g_len = g_len, L.b_off = b_off, L.b_len = b_len;
        L.alpha = a_len && bpp == 32 ? ((a_len >= 32 ? 0u : (1u << a_len)) - 1) << a_off : 0;
        if (bpp == 32 && r_off == 16 && g_off == 8 && b_off == 0) L.format = Format::Xrgb;
        else if (bpp == 32 && r_off == 0 && g_off == 8 && b_off == 16) L.format = Format::Xbgr;  // MediaTek and others
        else if (bpp == 16 && r_off == 11 && r_len == 5 && g_off == 5 && g_len == 6 && b_off == 0 && b_len == 5)
            L.format = Format::Rgb565;
        return L;
    }

    uint32_t pack(uint32_t xrgb) const {
        uint32_t r = (xrgb >> 16) & 0xFF, g = (xrgb >> 8) & 0xFF, b = xrgb & 0xFF;
        return (r >> (8 - r_len)) << r_off | (g >> (8 - g_len)) << g_off | (b >> (8 - b_len)) << b_off;
    }

    // `n` canvas pixels (XRGB8888) into framebuffer memory at `d`.
    void convert(uint8_t* d, const uint32_t* s, int n) const {
        switch (format) {
            case Format::Xrgb: {
                if (!alpha) {
                    std::memcpy(d, s, size_t(n) * 4);
                    return;
                }
                uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
                for (int x = 0; x < n; ++x) d32[x] = s[x] | alpha;
                return;
            }
            case Format::Xbgr: {
                uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
                for (int x = 0; x < n; ++x) {
                    uint32_t p = s[x];
                    d32[x] = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF) | alpha;
                }
                return;
            }
            case Format::Rgb565: {
                uint16_t* d16 = reinterpret_cast<uint16_t*>(d);
                for (int x = 0; x < n; ++x) {
                    uint32_t p = s[x];
                    d16[x] = uint16_t(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                }
                return;
            }
            case Format::Other:
                break;
        }
        if (bpp == 32) {
            uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
            for (int x = 0; x < n; ++x) d32[x] = pack(s[x]) | alpha;
        } else {
            uint16_t* d16 = reinterpret_cast<uint16_t*>(d);
            for (int x = 0; x < n; ++x) d16[x] = uint16_t(pack(s[x]));
        }
    }
};

// Copies a w x h canvas (rows `stride` pixels apart) into the framebuffer at
// `base` (rows `line_length` bytes apart). `shadow` holds what that memory
// holds; empty or of another size: everything is written. `rows` (row
// ranges [first, last), may be null) limits the comparison to rows that may
// have changed. Returns the pixels written.
inline size_t copy_changed(const uint32_t* src, size_t stride, int w, int h, std::vector<uint32_t>& shadow,
                           uint8_t* base, size_t line_length, const FbLayout& L,
                           const std::vector<std::pair<int, int>>* rows = nullptr) {
    if (w <= 0 || h <= 0) return 0;
    const bool full = shadow.size() != stride * size_t(h);
    if (full) shadow.assign(stride * size_t(h), 0);
    const size_t bpp = size_t(L.bpp / 8);
    size_t written = 0;
    std::vector<std::pair<int, int>> all{{0, h}};
    const auto& ranges = full || !rows ? all : *rows;
    for (const auto& [first, last] : ranges)
    for (int y = std::max(0, first); y < std::min(h, last); ++y) {
        const uint32_t* s = src + size_t(y) * stride;
        uint32_t* sh = shadow.data() + size_t(y) * stride;
        int x0 = 0, x1 = w;  // the changed span [x0, x1)
        if (!full) {
            while (x0 < w && s[x0] == sh[x0]) ++x0;
            if (x0 == w) continue;  // row unchanged
            while (x1 > x0 && s[x1 - 1] == sh[x1 - 1]) --x1;
        }
        if (x1 <= x0) continue;
        const size_t n = size_t(x1 - x0);
        std::memcpy(sh + x0, s + x0, n * 4);
        L.convert(base + size_t(y) * line_length + size_t(x0) * bpp, s + x0, int(n));
        written += n;
    }
    return written;
}

}  // namespace facet::platform
