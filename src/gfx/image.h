// Pictures: PNG and JPEG files decoded to 32-bit pixels (stb_image), with a
// copy scaled to the size they are drawn at (made once per size, averaged
// over the source pixels it covers, so posters and logos stay smooth).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace facet::gfx {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // 0xAARRGGBB, not premultiplied
    bool opaque = true;
    uint64_t id = 0;           // new for every decoded picture (partial redraw tells them apart)

    // The picture (or the part of it given in fractions of its size) scaled
    // to sw x sh, kept until another size or part is asked for.
    const std::vector<uint32_t>& scaled(int sw, int sh, float u0 = 0, float v0 = 0, float u1 = 1,
                                        float v1 = 1) const;

private:
    mutable std::vector<uint32_t> scaled_;
    mutable int sw_ = 0, sh_ = 0;
    mutable float crop_[4] = {0, 0, 1, 1};
};

// Decodes a PNG or JPEG file. Pictures over `max_side` pixels a side or
// files over 16 MB are refused, so a module cannot make the core decode
// something huge.
bool decode_image(const std::string& path, Image& out, std::string& error, int max_side = 4096);

}  // namespace facet::gfx
