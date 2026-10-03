// Software canvas: an XRGB8888 pixel buffer with clipping, alpha blending,
// vector shapes and text.
#pragma once

#include <string_view>
#include <vector>

#include "gfx/raster.h"
#include "gfx/types.h"

namespace facet::gfx {

class Font;
class Recorder;

enum class Align { Start, Center, End };

class Canvas {
public:
    void resize(int w, int h);
    // Draws into memory owned by someone else (e.g. shared with the GPU
    // helper) instead of its own buffer; w * h pixels.
    void use_external(uint32_t* memory, int w, int h);
    // GPU drawing: no pixels; every draw becomes a command in `rec`.
    void record_into(Recorder* rec, int w, int h);
    bool recording() const { return rec_ != nullptr; }
    int width() const { return w_; }
    int height() const { return h_; }
    const uint32_t* pixels() const { return px_; }
    uint32_t* pixels() { return px_; }

    // Clip stack: push intersects with the current clip.
    void push_clip(const Rect& r);
    void pop_clip();
    const Rect& clip() const { return clip_; }

    void clear(Color c);
    // Makes the rectangle see-through (alpha 0): with GPU layers, a plugin
    // surface drawn under the canvas shows there.
    void punch(const Rect& r);
    void fill_rect(const Rect& r, Color c);
    void fill_path(const Path& p, Color c);
    void fill_round_rect(const Rect& r, float radius, Color c);
    void fill_circle(float cx, float cy, float radius, Color c);
    // Copies an XRGB8888 image scaled into `dst` (nearest neighbour; fast
    // enough for full-screen video), clipped. `stride` is in pixels.
    void draw_pixels(const uint32_t* src, int w, int h, int stride, const Rect& dst);
    // Recording only: the frame's plugin surface number `layer`, drawn in order.
    void draw_layer(uint32_t layer);

    // Draws UTF-8 text with its baseline at y. Returns the advance width.
    float draw_text(Font& font, int px, float x, float baseline, std::string_view text, Color c);
    // Draws text vertically centred in `box`, aligned horizontally.
    void draw_text_in(Font& font, int px, const Rect& box, std::string_view text, Color c,
                      Align align = Align::Start);

private:
    void blend(int x, int y, Color c, float coverage);
    // Pixels [x0, x1) of row y with one coverage: a copy when opaque.
    void blend_span(int x0, int x1, int y, Color c, float coverage);

    int w_ = 0, h_ = 0;
    std::vector<uint32_t> own_;
    uint32_t* px_ = nullptr;
    Rect clip_;
    std::vector<Rect> clip_stack_;
    Rasterizer raster_;
    Recorder* rec_ = nullptr;
};

}  // namespace facet::gfx
