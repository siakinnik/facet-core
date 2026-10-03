#include "gfx/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gfx/font.h"
#include "gfx/recorder.h"
#include "gfx/utf8.h"

namespace facet::gfx {

void Canvas::resize(int w, int h) {
    w_ = w;
    h_ = h;
    own_.assign(size_t(w) * size_t(h), 0xFF000000u);
    px_ = own_.data();
    clip_ = {0, 0, float(w), float(h)};
    clip_stack_.clear();
}

void Canvas::use_external(uint32_t* memory, int w, int h) {
    own_.clear();
    own_.shrink_to_fit();
    w_ = w;
    h_ = h;
    px_ = memory;
    std::fill(px_, px_ + size_t(w) * size_t(h), 0xFF000000u);
    clip_ = {0, 0, float(w), float(h)};
    clip_stack_.clear();
}

void Canvas::record_into(Recorder* rec, int w, int h) {
    own_.clear();
    own_.shrink_to_fit();
    rec_ = rec;
    w_ = w;
    h_ = h;
    px_ = nullptr;
    clip_ = {0, 0, float(w), float(h)};
    clip_stack_.clear();
}

void Canvas::punch(const Rect& r) {
    if (rec_) return;  // surfaces are drawn in order: no holes needed
    Rect cr = r.intersect(clip_);
    if (cr.empty()) return;
    int x0 = std::max(0, int(std::floor(cr.x))), y0 = std::max(0, int(std::floor(cr.y)));
    int x1 = std::min(w_, int(std::ceil(cr.right()))), y1 = std::min(h_, int(std::ceil(cr.bottom())));
    for (int y = y0; y < y1; ++y) std::fill(px_ + size_t(y) * size_t(w_) + x0, px_ + size_t(y) * size_t(w_) + x1, 0u);
}

void Canvas::push_clip(const Rect& r) {
    clip_stack_.push_back(clip_);
    clip_ = clip_.intersect(r);
}

void Canvas::pop_clip() {
    if (clip_stack_.empty()) return;
    clip_ = clip_stack_.back();
    clip_stack_.pop_back();
}

void Canvas::clear(Color c) {
    if (rec_) {
        rec_->clip({0, 0, float(w_), float(h_)});
        rec_->rect({0, 0, float(w_), float(h_)}, 0, Color{c.r, c.g, c.b, 255});
        return;
    }
    uint32_t v = c.xrgb();
    std::fill(px_, px_ + size_t(w_) * size_t(h_), v);
}

inline void Canvas::blend(int x, int y, Color c, float coverage) {
    uint32_t& d = px_[size_t(y) * w_ + x];
    unsigned a = unsigned(coverage * c.a + 0.5f);
    if (a >= 255) {
        d = c.xrgb();
        return;
    }
    unsigned ia = 255 - a;
    unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    dr = (c.r * a + dr * ia + 127) / 255;
    dg = (c.g * a + dg * ia + 127) / 255;
    db = (c.b * a + db * ia + 127) / 255;
    d = 0xFF000000u | dr << 16 | dg << 8 | db;
}

void Canvas::blend_span(int x0, int x1, int y, Color c, float coverage) {
    if (x1 <= x0) return;
    uint32_t* d = px_ + size_t(y) * size_t(w_);
    unsigned a = unsigned(coverage * c.a + 0.5f);
    if (a == 0) return;
    if (a >= 255) {
        std::fill(d + x0, d + x1, c.xrgb());
        return;
    }
    unsigned ia = 255 - a;
    unsigned sr = c.r * a + 127, sg = c.g * a + 127, sb = c.b * a + 127;
    for (int x = x0; x < x1; ++x) {
        uint32_t v = d[x];
        unsigned dr = (sr + ((v >> 16) & 0xFF) * ia) / 255;
        unsigned dg = (sg + ((v >> 8) & 0xFF) * ia) / 255;
        unsigned db = (sb + (v & 0xFF) * ia) / 255;
        d[x] = 0xFF000000u | dr << 16 | dg << 8 | db;
    }
}

void Canvas::fill_rect(const Rect& r, Color c) {
    if (c.a == 0) return;
    Rect cr = r.intersect(clip_);
    if (cr.empty()) return;
    if (rec_) {
        rec_->clip(clip_);
        rec_->rect(cr, 0, c);
        return;
    }
    // Sub-pixel edges are anti-aliased by treating them as partial coverage;
    // the whole pixels in between are one span per row.
    int x0 = int(std::floor(cr.x)), y0 = int(std::floor(cr.y));
    int x1 = int(std::ceil(cr.right())), y1 = int(std::ceil(cr.bottom()));
    x1 = std::min(x1, w_);
    y1 = std::min(y1, h_);
    int ix0 = int(std::ceil(cr.x)), ix1 = std::min(int(std::floor(cr.right())), w_);
    for (int y = y0; y < y1; ++y) {
        float cy = std::min(float(y + 1), cr.bottom()) - std::max(float(y), cr.y);
        if (cy <= 0.f) continue;
        if (ix0 >= ix1) {  // narrower than a pixel
            for (int x = x0; x < x1; ++x) {
                float cx = std::min(float(x + 1), cr.right()) - std::max(float(x), cr.x);
                if (cx * cy > 0.f) blend(x, y, c, cx * cy);
            }
            continue;
        }
        if (x0 < ix0) blend(x0, y, c, (float(ix0) - cr.x) * cy);
        blend_span(ix0, ix1, y, c, cy);
        if (ix1 < x1) blend(ix1, y, c, (cr.right() - float(ix1)) * cy);
    }
}

void Canvas::draw_layer(uint32_t layer) {
    if (!rec_) return;
    rec_->clip(clip_);
    rec_->surface(layer);
}

void Canvas::draw_pixels(const uint32_t* src, int w, int h, int stride, const Rect& dst) {
    if (!src || w <= 0 || h <= 0 || dst.empty()) return;
    Rect cr = dst.intersect(clip_).intersect({0, 0, float(w_), float(h_)});
    if (cr.empty()) return;
    if (rec_) {
        rec_->clip(clip_);
        rec_->image(src, w, h, stride, dst);
        return;
    }
    int x0 = int(std::floor(cr.x)), y0 = int(std::floor(cr.y));
    int x1 = int(std::ceil(cr.right())), y1 = int(std::ceil(cr.bottom()));
    float sx = float(w) / dst.w, sy = float(h) / dst.h;
    // Source column per destination column, computed once per call.
    std::vector<int> cols(size_t(x1 - x0));
    for (int x = x0; x < x1; ++x) cols[size_t(x - x0)] = std::clamp(int((float(x) + 0.5f - dst.x) * sx), 0, w - 1);
    for (int y = y0; y < y1; ++y) {
        int srow = std::clamp(int((float(y) + 0.5f - dst.y) * sy), 0, h - 1);
        const uint32_t* s = src + size_t(srow) * size_t(stride);
        uint32_t* d = px_ + size_t(y) * size_t(w_);
        if (w == x1 - x0 && x0 == int(dst.x) && sx == 1.f) {
            std::memcpy(d + x0, s, size_t(w) * 4);  // 1:1 row
            continue;
        }
        for (int x = x0; x < x1; ++x) d[x] = s[cols[size_t(x - x0)]];
    }
}

void Canvas::fill_path(const Path& p, Color c) {
    if (p.empty() || c.a == 0) return;
    Rect b = p.bounds();
    if (rec_) {
        // The mask covers the shape's whole bounds on the canvas (the clip
        // is applied when drawing), so it can be reused while scrolling.
        Rect full = Rect{std::floor(b.x), std::floor(b.y), std::ceil(b.w) + 2, std::ceil(b.h) + 2}.intersect(
            {0, 0, float(w_), float(h_)});
        if (full.empty() || full.intersect(clip_).empty()) return;
        int ox = int(full.x), oy = int(full.y);
        int rw = std::min(int(std::ceil(full.right())), w_) - ox, rh = std::min(int(std::ceil(full.bottom())), h_) - oy;
        if (rw <= 0 || rh <= 0) return;
        rec_->clip(clip_);
        rec_->path(p, ox, oy, rw, rh, c);
        return;
    }
    Rect region = Rect{std::floor(b.x), std::floor(b.y), std::ceil(b.w) + 2, std::ceil(b.h) + 2}.intersect(clip_);
    if (region.empty()) return;
    int ox = int(std::floor(region.x)), oy = int(std::floor(region.y));
    int rw = int(std::ceil(region.right())) - ox, rh = int(std::ceil(region.bottom())) - oy;
    rw = std::min(rw, w_ - ox);
    rh = std::min(rh, h_ - oy);
    if (rw <= 0 || rh <= 0) return;
    raster_.reset(rw, rh);
    rasterize(p, raster_, -float(ox), -float(oy));
    raster_.for_each_span([&](int x0, int x1, int y, float cov) { blend_span(ox + x0, ox + x1, oy + y, c, cov); });
}

void Canvas::fill_round_rect(const Rect& r, float radius, Color c) {
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) * 0.5f);
    if (rec_) {
        if (c.a == 0 || r.empty() || r.intersect(clip_).empty()) return;
        rec_->clip(clip_);
        rec_->rect(r, radius, c);
        return;
    }
    // Only the rows with the corners need the rasterizer; the rows between
    // them are a plain rectangle (cards and buttons are mostly that).
    float top = std::ceil(r.y + radius), bottom = std::floor(r.bottom() - radius);
    Path p;
    p.round_rect(r, radius);
    if (bottom - top < 8.f) {
        fill_path(p, c);
        return;
    }
    push_clip({clip_.x, clip_.y, clip_.w, top - clip_.y});
    fill_path(p, c);
    pop_clip();
    push_clip({clip_.x, bottom, clip_.w, clip_.bottom() - bottom});
    fill_path(p, c);
    pop_clip();
    fill_rect({r.x, top, r.w, bottom - top}, c);
}

void Canvas::fill_circle(float cx, float cy, float radius, Color c) {
    if (rec_) return fill_round_rect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c);
    Path p;
    p.circle(cx, cy, radius);
    fill_path(p, c);
}

float Canvas::draw_text(Font& font, int px, float x, float baseline, std::string_view text, Color c) {
    float pen = x;
    int by = int(std::lround(baseline));
    int cx0 = int(std::floor(clip_.x)), cy0 = int(std::floor(clip_.y));
    int cx1 = int(std::ceil(clip_.right())), cy1 = int(std::ceil(clip_.bottom()));
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = utf8_next(text, i);
        const Glyph& g = font.glyph(cp, px);
        int gx = int(std::lround(pen)) + g.left;
        int gy = by - g.top;
        if (rec_) {
            if (g.w > 0 && gx < cx1 && gy < cy1 && gx + g.w > cx0 && gy + g.h > cy0) {
                rec_->clip(clip_);
                rec_->glyph(font, cp, px, gx, gy, c);
            }
            pen += g.advance;
            continue;
        }
        for (int y = 0; y < g.h; ++y) {
            int dy = gy + y;
            if (dy < cy0 || dy >= cy1) continue;
            const uint8_t* row = &g.alpha[size_t(y) * g.w];
            for (int x = 0; x < g.w; ++x) {
                int dx = gx + x;
                if (dx < cx0 || dx >= cx1 || row[x] == 0) continue;
                blend(dx, dy, c, row[x] / 255.f);
            }
        }
        pen += g.advance;
    }
    return pen - x;
}

void Canvas::draw_text_in(Font& font, int px, const Rect& box, std::string_view text, Color c, Align align) {
    float w = font.measure(text, px);
    float x = box.x;
    if (align == Align::Center) x = box.cx() - w * 0.5f;
    else if (align == Align::End) x = box.right() - w;
    // Optical centre: cap height is roughly 0.7 of the em box.
    float baseline = box.cy() + font.cap_height(px) * 0.5f;
    draw_text(font, px, x, baseline, text, c);
}

}  // namespace facet::gfx
