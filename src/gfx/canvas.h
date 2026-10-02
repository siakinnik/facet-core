// Software canvas: an XRGB8888 pixel buffer with clipping, alpha blending,
// vector shapes and text.
#pragma once

#include <string_view>
#include <vector>

#include "gfx/raster.h"
#include "gfx/types.h"

namespace facet::gfx {

class Font;

enum class Align { Start, Center, End };

class Canvas {
public:
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    const uint32_t* pixels() const { return px_.data(); }
    uint32_t* pixels() { return px_.data(); }

    // Clip stack: push intersects with the current clip.
    void push_clip(const Rect& r);
    void pop_clip();
    const Rect& clip() const { return clip_; }

    void clear(Color c);
    void fill_rect(const Rect& r, Color c);
    void fill_path(const Path& p, Color c);
    void fill_round_rect(const Rect& r, float radius, Color c);
    void fill_circle(float cx, float cy, float radius, Color c);
    // Copies an XRGB8888 image scaled into `dst` (nearest neighbour; fast
    // enough for full-screen video), clipped. `stride` is in pixels.
    void draw_pixels(const uint32_t* src, int w, int h, int stride, const Rect& dst);

    // Draws UTF-8 text with its baseline at y. Returns the advance width.
    float draw_text(Font& font, int px, float x, float baseline, std::string_view text, Color c);
    // Draws text vertically centred in `box`, aligned horizontally.
    void draw_text_in(Font& font, int px, const Rect& box, std::string_view text, Color c,
                      Align align = Align::Start);

private:
    void blend(int x, int y, Color c, float coverage);

    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
    Rect clip_;
    std::vector<Rect> clip_stack_;
    Rasterizer raster_;
};

}  // namespace facet::gfx
