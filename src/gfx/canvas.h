// Software canvas: an XRGB8888 pixel buffer with clipping, alpha blending,
// vector shapes and text.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/raster.h"
#include "gfx/types.h"

namespace facet::gfx {

class Font;
class Recorder;
struct Image;

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
    // Light around `r` (GPU only, when recording(); the processor builds its own).
    void glow(const Rect& r, float radius, float spread, Color c);
    // A part that rarely changes (a theme's background). With the GPU it is kept as a
    // texture while `stamp` stays the same: false means it is there already and nothing
    // is to be drawn; true (always, with the processor): draw it, then call layer_end().
    bool layer_begin(uint32_t id, uint64_t stamp, const Rect& r);
    void layer_end();
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
    void fill_ellipse(const Rect& box, Color c);
    // GPU only (when recording()): the processor draws strips, see ui/canvas_ops.cpp.
    void gradient(const Rect& r, float radius, Color from, Color to, bool vertical);
    void fill_circle(float cx, float cy, float radius, Color c);
    // Copies an XRGB8888 image scaled into `dst` (nearest neighbour; fast
    // enough for full-screen video), clipped. `stride` is in pixels.
    void draw_pixels(const uint32_t* src, int w, int h, int stride, const Rect& dst);
    // A picture scaled into `dst` (its own aspect ratio is the caller's
    // business), with its transparency, rounded corners of `radius` and an
    // overall `alpha`. With the GPU it is drawn opaque, without corners.
    // `img` must stay alive until the frame is presented.
    // `crop`: the part of the picture shown, in fractions of its size.
    void draw_image(const Image& img, const Rect& dst, float radius = 0, float alpha = 1,
                    const Rect& crop = Rect{0, 0, 1, 1});
    // Recording only: the frame's plugin surface number `layer`, drawn in order.
    void draw_layer(uint32_t layer);

    // Draws UTF-8 text with its baseline at y. Returns the advance width.
    float draw_text(Font& font, int px, float x, float baseline, std::string_view text, Color c);
    // Draws text vertically centred in `box`, aligned horizontally.
    void draw_text_in(Font& font, int px, const Rect& box, std::string_view text, Color c,
                      Align align = Align::Start);

    // ---- Partial redraw (processor drawing, slow devices). Between
    // begin_frame() and end_frame() drawing is only recorded; end_frame()
    // compares it with the last frame on a grid of tiles and draws just the
    // tiles whose drawing changed, so a key press costs the key, not the
    // screen. take_damage() is every area drawn since it was last called
    // (what the screen has to be sent).
    void set_partial(bool on);
    bool partial() const { return partial_; }
    void begin_frame();
    void end_frame();
    void invalidate() { full_ = true; }  // the next end_frame() draws everything
    std::vector<Rect> take_damage();
    struct FrameStats {
        size_t commands = 0;
        double area = 0;  // share of the canvas drawn, 0..1
        size_t mismatched = 0;  // FACET_CHECK_PARTIAL: pixels unlike a full redraw
    };
    const FrameStats& frame_stats() const { return stats_; }

private:
    enum class CmdKind : uint8_t { Clear, Rect, Path, Pixels, Text, RoundRect, Circle, Image };
    struct Cmd {
        explicit Cmd(CmdKind k) : kind(k) {}
        CmdKind kind;
        Rect clip, bounds;
        Color c;
        uint64_t hash = 0;
        Rect r;              // Rect, Pixels (destination)
        uint32_t index = 0;  // Path: into paths_; Text: offset into text_
        uint32_t len = 0;    // Text: bytes
        Font* font = nullptr;
        int px = 0;
        float x = 0, y = 0;  // Text: pen and baseline; Circle: centre
        float radius = 0;    // RoundRect, Circle, Image
        float alpha = 1;     // Image
        Rect crop{0, 0, 1, 1};
        const Image* image = nullptr;
        const uint32_t* src = nullptr;
        int w = 0, h = 0, stride = 0;
    };
    void record(Cmd& c);
    void replay(const Cmd& c);
    void clear_now(Color c);
    void draw_image_now(const Image& img, const Rect& dst, float radius, float alpha, const Rect& crop);

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

    bool partial_ = false, deferring_ = false, full_ = true;
    bool replaying_ = false;  // drawing a tile: clip_ is the tile part of replay_clip_
    Rect replay_clip_;
    std::vector<Cmd> cmds_;
    std::vector<Path> paths_;
    size_t paths_used_ = 0;
    std::string text_;
    std::vector<uint64_t> tiles_, prev_tiles_;
    std::vector<Rect> damage_;  // since take_damage()
    uint64_t serial_ = 0;       // frames: pixels from elsewhere count as changed every frame
    FrameStats stats_;
};

}  // namespace facet::gfx
