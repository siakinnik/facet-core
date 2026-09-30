// Self-contained TrueType (glyf) font loader and rasterizer with a glyph cache.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx/raster.h"

namespace facet::gfx {

struct Glyph {
    int w = 0, h = 0;
    int left = 0, top = 0;  // bitmap offset from pen position / baseline
    float advance = 0;
    std::vector<uint8_t> alpha;
};

class Font {
public:
    bool load(const std::string& path);
    const std::string& path() const { return path_; }

    const Glyph& glyph(uint32_t codepoint, int px);
    float measure(std::string_view utf8, int px);
    float ascent(int px) const { return ascent_ * scale(px); }
    float descent(int px) const { return -descent_ * scale(px); }  // positive
    float cap_height(int px) const { return cap_height_ * scale(px); }

private:
    struct Matrix {
        float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };
    float scale(int px) const { return float(px) / units_per_em_; }
    uint16_t glyph_index(uint32_t cp) const;
    float advance_units(uint16_t gid) const;
    bool glyph_offset(uint16_t gid, uint32_t& off, uint32_t& len) const;
    void outline(uint16_t gid, const Matrix& m, Path& out, int depth) const;

    uint8_t u8(uint32_t o) const { return o < data_.size() ? data_[o] : 0; }
    uint16_t u16(uint32_t o) const { return uint16_t(u8(o) << 8 | u8(o + 1)); }
    int16_t i16(uint32_t o) const { return int16_t(u16(o)); }
    uint32_t u32(uint32_t o) const { return uint32_t(u16(o)) << 16 | u16(o + 2); }

    std::string path_;
    std::vector<uint8_t> data_;
    uint32_t cmap_ = 0, glyf_ = 0, loca_ = 0, hmtx_ = 0;
    uint32_t cmap_format_ = 0;
    int num_hmetrics_ = 0, num_glyphs_ = 0, loca_long_ = 0;
    float units_per_em_ = 1000, ascent_ = 800, descent_ = -200, cap_height_ = 700;

    std::unordered_map<uint64_t, Glyph> cache_;
    Rasterizer raster_;
};

}  // namespace facet::gfx
