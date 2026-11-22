#include "gfx/image.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace facet::gfx {

namespace {
std::atomic<uint64_t> g_ids{0};
}

bool decode_image(const std::string& path, Image& out, std::string& error, int max_side) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "no such file";
        return false;
    }
    if (st.st_size > 16 * 1024 * 1024) {
        error = "file too large";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open";
        return false;
    }
    std::vector<unsigned char> data(size_t(st.st_size));
    size_t n = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data.data(), int(n), &w, &h, &comp) || w <= 0 || h <= 0) {
        error = "not a PNG or JPEG picture";
        return false;
    }
    if (w > max_side || h > max_side) {
        error = "picture too large";
        return false;
    }
    unsigned char* rgba = stbi_load_from_memory(data.data(), int(n), &w, &h, &comp, 4);
    if (!rgba) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "cannot decode";
        return false;
    }
    out.w = w, out.h = h;
    out.px.resize(size_t(w) * size_t(h));
    out.opaque = true;
    for (size_t i = 0; i < out.px.size(); ++i) {
        const unsigned char* p = rgba + i * 4;
        out.px[i] = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
        if (p[3] != 255) out.opaque = false;
    }
    stbi_image_free(rgba);
    out.id = ++g_ids;
    return true;
}

const std::vector<uint32_t>& Image::scaled(int sw, int sh, float u0, float v0, float u1, float v1) const {
    const bool whole = u0 <= 0 && v0 <= 0 && u1 >= 1 && v1 >= 1;
    if (whole && sw == w && sh == h) return px;
    if (sw == sw_ && sh == sh_ && !scaled_.empty() && crop_[0] == u0 && crop_[1] == v0 && crop_[2] == u1 &&
        crop_[3] == v1)
        return scaled_;
    sw_ = sw, sh_ = sh;
    crop_[0] = u0, crop_[1] = v0, crop_[2] = u1, crop_[3] = v1;
    // The source part in pixels.
    const int sx0 = std::clamp(int(u0 * float(w)), 0, w - 1), sy0 = std::clamp(int(v0 * float(h)), 0, h - 1);
    const int pw = std::max(1, std::min(w - sx0, int(std::ceil((u1 - u0) * float(w))))),
              ph = std::max(1, std::min(h - sy0, int(std::ceil((v1 - v0) * float(h)))));
    scaled_.assign(size_t(sw) * size_t(sh), 0);
    // Each destination pixel averages the source pixels it covers (colours
    // weighted by alpha, so transparent edges do not darken); enlarging
    // falls back to the nearest pixel.
    for (int y = 0; y < sh; ++y) {
        int y0 = sy0 + int(int64_t(y) * ph / sh), y1 = std::max(y0 + 1, sy0 + int(int64_t(y + 1) * ph / sh));
        for (int x = 0; x < sw; ++x) {
            int x0 = sx0 + int(int64_t(x) * pw / sw), x1 = std::max(x0 + 1, sx0 + int(int64_t(x + 1) * pw / sw));
            uint64_t a = 0, r = 0, g = 0, b = 0, count = 0;
            for (int yy = y0; yy < y1 && yy < h; ++yy)
                for (int xx = x0; xx < x1 && xx < w; ++xx) {
                    uint32_t p = px[size_t(yy) * size_t(w) + size_t(xx)];
                    uint32_t pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                    ++count;
                }
            uint32_t out = 0;
            if (a > 0)
                out = uint32_t(a / count) << 24 | uint32_t(r / a) << 16 | uint32_t(g / a) << 8 | uint32_t(b / a);
            scaled_[size_t(y) * size_t(sw) + size_t(x)] = out;
        }
    }
    return scaled_;
}

}  // namespace facet::gfx
