// Records a frame as drawing commands (gfx/ops.h) for the GPU helper and
// keeps track of the mask atlas the helper holds: which glyphs and shapes
// are uploaded already and where.
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "gfx/ops.h"
#include "gfx/raster.h"
#include "gfx/types.h"

namespace facet::gfx {

class Font;

class Recorder {
public:
    // Starts a frame. `lost`: the helper restarted and forgot the atlas.
    void begin_frame(bool lost);
    // The frame went to the helper (otherwise the next one repeats its uploads).
    void sent() { sent_ = true; }
    const std::vector<uint8_t>& data() const { return buf_; }

    void clip(const Rect& r);
    void rect(const Rect& r, float radius, Color c);
    // A glyph's bitmap at an integer position.
    void glyph(Font& font, uint32_t codepoint, int px, int x, int y, Color c);
    // A path's coverage within `bounds` (canvas pixels, integers).
    void path(const Path& p, int ox, int oy, int w, int h, Color c);
    void surface(uint32_t layer);
    void image(const uint32_t* src, int w, int h, int stride, const Rect& dst);

    size_t atlas_pages() const { return pages_.size(); }

private:
    struct Entry {
        uint32_t page;
        int u, v, w, h;
    };
    struct Shelf {
        int y, h, x;
    };
    struct Page {
        std::vector<Shelf> shelves;
        int bottom = 0;
    };

    void* add(uint32_t type, size_t fixed, size_t payload);
    // Places w x h in the atlas and uploads `alpha`; false when it is full.
    bool place(uint64_t key, const uint8_t* alpha, int w, int h, Entry& out);
    void mask(const Entry& e, int x, int y, Color c);
    void inline_mask(const uint8_t* alpha, int w, int h, int x, int y, Color c);
    void reset_atlas();
    void keep(const void* record);  // an atlas change, repeated if this frame is never sent

    std::vector<uint8_t> buf_, carry_;
    bool sent_ = true;
    std::vector<Page> pages_;
    std::unordered_map<uint64_t, Entry> entries_;
    Rect clip_{-1, -1, -1, -1};
    bool clip_sent_ = false;
    Rasterizer raster_;
    std::vector<uint8_t> scratch_;
};

}  // namespace facet::gfx
