// Drawing commands: what the canvas records instead of pixels when the GPU
// helper draws Facet's interface (GPU mode). The core writes them into
// shared memory, facet-gpu turns them into OpenGL draws. Both sides are
// built from this header for the same machine, so the records are plain
// structs: a header with the type and size, a fixed part and, for some, a
// payload padded to 4 bytes.
//
// Text glyphs and vector shapes (icons, rings) arrive as alpha masks: the
// core rasterizes them once with its own rasterizer and uploads them into
// mask atlas pages kept by the helper; rectangles and rounded rectangles are
// drawn by a shader directly. Coordinates are canvas pixels, origin top left.
#pragma once

#include <cstdint>

namespace facet::gfx::ops {

enum Type : uint32_t {
    kClip = 1,     // Clip: what the following draws are limited to
    kRect,         // Rect: a (rounded) rectangle, anti-aliased edges
    kMask,         // Mask: an atlas region (or inline alpha) tinted with a colour
    kUpload,       // Upload: alpha bytes into an atlas page
    kAtlasReset,   // Header only: every atlas page is free again
    kSurface,      // Surface: a plugin surface (the frame's layer list), opaque
    kImage,        // Image: XRGB8888 pixels scaled into a rectangle (nearest)
};

constexpr int kAtlasSize = 2048;       // atlas pages are kAtlasSize x kAtlasSize alpha
constexpr int kMaxAtlasPages = 8;
constexpr uint32_t kInlinePage = 0xFFFFFFFFu;  // Mask: the alpha bytes follow the record

struct Header {
    uint32_t type, size;  // size: the whole record with payload, a multiple of 4
};

// Colours: r | g << 8 | b << 16 | a << 24 (straight alpha).
struct Clip {
    Header h;
    float x, y, w, h_;
};

struct Rect {
    Header h;
    float x, y, w, h_, radius;
    uint32_t color;
};

struct Mask {
    Header h;
    int32_t x, y;  // where the mask's top left pixel goes
    uint32_t page;
    int32_t u, v, w, h_;  // region in the page (u, v unused for inline masks)
    uint32_t color;
};

struct Upload {
    Header h;
    uint32_t page;
    int32_t u, v, w, h_;  // followed by w * h bytes
};

struct Surface {
    Header h;
    uint32_t layer;  // index into the frame's "layers"
};

struct Image {
    Header h;
    int32_t w, h_;  // followed by w * h XRGB8888 pixels
    float x, y, dw, dh;
};

}  // namespace facet::gfx::ops
