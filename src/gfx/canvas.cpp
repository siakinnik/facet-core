#include "gfx/canvas.h"

#include <cmath>

#include "gfx/font.h"
#include "gfx/utf8.h"

namespace facet::gfx {

void Canvas::resize(int w, int h) {
    w_ = w;
    h_ = h;
    px_.assign(size_t(w) * size_t(h), 0xFF000000u);
    clip_ = {0, 0, float(w), float(h)};
    clip_stack_.clear();
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
    uint32_t v = c.xrgb();
    std::fill(px_.begin(), px_.end(), v);
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

void Canvas::fill_rect(const Rect& r, Color c) {
    if (c.a == 0) return;
    Rect cr = r.intersect(clip_);
    if (cr.empty()) return;
    // Sub-pixel edges are anti-aliased by treating them as partial coverage.
    int x0 = int(std::floor(cr.x)), y0 = int(std::floor(cr.y));
    int x1 = int(std::ceil(cr.right())), y1 = int(std::ceil(cr.bottom()));
    x1 = std::min(x1, w_);
    y1 = std::min(y1, h_);
    for (int y = y0; y < y1; ++y) {
        float cy = std::min(float(y + 1), cr.bottom()) - std::max(float(y), cr.y);
        for (int x = x0; x < x1; ++x) {
            float cx = std::min(float(x + 1), cr.right()) - std::max(float(x), cr.x);
            float cov = cx * cy;
            if (cov > 0.f) blend(x, y, c, cov);
        }
    }
}

void Canvas::fill_path(const Path& p, Color c) {
    if (p.empty() || c.a == 0) return;
    Rect b = p.bounds();
    Rect region = Rect{std::floor(b.x), std::floor(b.y), std::ceil(b.w) + 2, std::ceil(b.h) + 2}.intersect(clip_);
    if (region.empty()) return;
    int ox = int(std::floor(region.x)), oy = int(std::floor(region.y));
    int rw = int(std::ceil(region.right())) - ox, rh = int(std::ceil(region.bottom())) - oy;
    rw = std::min(rw, w_ - ox);
    rh = std::min(rh, h_ - oy);
    if (rw <= 0 || rh <= 0) return;
    raster_.reset(rw, rh);
    rasterize(p, raster_, -float(ox), -float(oy));
    raster_.for_each([&](int x, int y, float cov) { blend(ox + x, oy + y, c, cov); });
}

void Canvas::fill_round_rect(const Rect& r, float radius, Color c) {
    Path p;
    p.round_rect(r, radius);
    fill_path(p, c);
}

void Canvas::fill_circle(float cx, float cy, float radius, Color c) {
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
