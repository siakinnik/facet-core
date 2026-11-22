#include "gfx/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gfx/font.h"
#include "gfx/image.h"
#include "gfx/recorder.h"
#include "gfx/utf8.h"

namespace facet::gfx {

void Canvas::resize(int w, int h) {
    full_ = true;
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
    if (deferring_) {
        Cmd cmd{CmdKind::Clear};
        cmd.c = c;
        cmd.bounds = {0, 0, float(w_), float(h_)};
        record(cmd);
        return;
    }
    clear_now(c);
}

// Fills the clip (all of the canvas outside a partial redraw).
void Canvas::clear_now(Color c) {
    uint32_t v = c.xrgb();
    if (clip_.x <= 0 && clip_.y <= 0 && clip_.right() >= float(w_) && clip_.bottom() >= float(h_)) {
        std::fill(px_, px_ + size_t(w_) * size_t(h_), v);
        return;
    }
    int x0 = std::max(0, int(std::floor(clip_.x))), y0 = std::max(0, int(std::floor(clip_.y)));
    int x1 = std::min(w_, int(std::ceil(clip_.right()))), y1 = std::min(h_, int(std::ceil(clip_.bottom())));
    for (int y = y0; y < y1; ++y) std::fill(px_ + size_t(y) * size_t(w_) + x0, px_ + size_t(y) * size_t(w_) + x1, v);
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
    // Red and blue are blended in one multiply, green in another; x/255 rounded is
    // ((x + 128) + ((x + 128) >> 8)) >> 8 for x up to 255*255.
    unsigned ia = 255 - a;
    const uint32_t src_rb = ((uint32_t(c.r) << 16 | c.b) * a) + 0x00800080u;
    const uint32_t src_g = uint32_t(c.g) * a + 0x80u;
    for (int x = x0; x < x1; ++x) {
        uint32_t v = d[x];
        uint32_t rb = src_rb + (v & 0x00FF00FFu) * ia;
        rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        uint32_t g = src_g + ((v >> 8) & 0xFFu) * ia;
        g = ((g + (g >> 8)) >> 8) & 0xFFu;
        d[x] = 0xFF000000u | rb | g << 8;
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
    if (deferring_) {
        Cmd cmd{CmdKind::Rect};
        cmd.c = c;
        cmd.r = r;
        cmd.bounds = cr;
        record(cmd);
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
    if (deferring_) {
        Cmd cmd{CmdKind::Pixels};
        cmd.r = dst;
        cmd.bounds = cr;
        cmd.src = src, cmd.w = w, cmd.h = h, cmd.stride = stride;
        record(cmd);
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

void Canvas::draw_image(const Image& img, const Rect& dst, float radius, float alpha, const Rect& crop) {
    if (img.w <= 0 || img.h <= 0 || dst.w < 1 || dst.h < 1 || alpha <= 0.f) return;
    Rect cr = dst.intersect(clip_).intersect({0, 0, float(w_), float(h_)});
    if (cr.empty()) return;
    if (rec_) {
        int dw = std::max(1, int(std::lround(dst.w))), dh = std::max(1, int(std::lround(dst.h)));
        const auto& s = img.scaled(dw, dh, crop.x, crop.y, crop.right(), crop.bottom());
        rec_->clip(clip_);
        rec_->image(s.data(), dw, dh, dw, dst);
        return;
    }
    if (deferring_) {
        Cmd cmd(CmdKind::Image);
        cmd.bounds = cr;
        cmd.image = &img;
        cmd.r = dst;
        cmd.radius = std::clamp(radius, 0.f, std::min(dst.w, dst.h) * 0.5f);
        cmd.alpha = std::min(alpha, 1.f);
        cmd.crop = crop;
        record(cmd);
        return;
    }
    draw_image_now(img, dst, std::clamp(radius, 0.f, std::min(dst.w, dst.h) * 0.5f), std::min(alpha, 1.f), crop);
}

void Canvas::draw_image_now(const Image& img, const Rect& dst, float radius, float alpha, const Rect& crop) {
    Rect cr = dst.intersect(clip_).intersect({0, 0, float(w_), float(h_)});
    if (cr.empty()) return;
    const int dw = std::max(1, int(std::lround(dst.w))), dh = std::max(1, int(std::lround(dst.h)));
    const auto& s = img.scaled(dw, dh, crop.x, crop.y, crop.right(), crop.bottom());
    const int ox = int(std::lround(dst.x)), oy = int(std::lround(dst.y));
    const int x0 = std::max(0, int(std::floor(cr.x))), x1 = std::min(w_, int(std::ceil(cr.right())));
    const int y0 = std::max(0, int(std::floor(cr.y))), y1 = std::min(h_, int(std::ceil(cr.bottom())));
    const float rx0 = float(ox), ry0 = float(oy), rx1 = float(ox + dw), ry1 = float(oy + dh);
    for (int y = y0; y < y1; ++y) {
        const int sy = std::clamp(y - oy, 0, dh - 1);
        const uint32_t* row = s.data() + size_t(sy) * size_t(dw);
        uint32_t* d = px_ + size_t(y) * size_t(w_);
        const float cy = float(y) + 0.5f;
        const bool corner_row = radius > 0 && (cy < ry0 + radius || cy > ry1 - radius);
        for (int x = x0; x < x1; ++x) {
            const uint32_t p = row[std::clamp(x - ox, 0, dw - 1)];
            float cov = alpha;
            if (corner_row) {
                const float cx = float(x) + 0.5f;
                float dx = std::max({0.f, rx0 + radius - cx, cx - (rx1 - radius)});
                float dy = std::max({0.f, ry0 + radius - cy, cy - (ry1 - radius)});
                if (dx > 0 && dy > 0) cov *= std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.f, 1.f);
            }
            const uint32_t pa = p >> 24;
            if (pa == 255 && cov >= 1.f) {
                d[x] = p | 0xFF000000u;
                continue;
            }
            if (pa == 0 || cov <= 0.f) continue;
            blend(x, y, Color{uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p), uint8_t(pa)}, cov);
        }
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
    // Coverage adds up along each row from the left: a shape cut by a
    // partial redraw's tile is rasterized as wide as it was drawn (rows may be
    // cut), and only the pixels inside the tile are written.
    Rect raster_clip = clip_;
    if (replaying_) raster_clip = Rect{replay_clip_.x, clip_.y, replay_clip_.w, clip_.h};
    Rect region = Rect{std::floor(b.x), std::floor(b.y), std::ceil(b.w) + 2, std::ceil(b.h) + 2}.intersect(raster_clip);
    if (region.empty()) return;
    if (deferring_) {
        Cmd cmd{CmdKind::Path};
        cmd.c = c;
        cmd.bounds = region;
        if (paths_used_ == paths_.size()) paths_.emplace_back();
        paths_[paths_used_] = p;
        cmd.index = uint32_t(paths_used_++);
        record(cmd);
        return;
    }
    int ox = int(std::floor(region.x)), oy = int(std::floor(region.y));
    int rw = int(std::ceil(region.right())) - ox, rh = int(std::ceil(region.bottom())) - oy;
    rw = std::min(rw, w_ - ox);
    rh = std::min(rh, h_ - oy);
    if (rw <= 0 || rh <= 0) return;
    raster_.reset(rw, rh);
    rasterize(p, raster_, -float(ox), -float(oy));
    if (!replaying_) {
        raster_.for_each_span([&](int x0, int x1, int y, float cov) { blend_span(ox + x0, ox + x1, oy + y, c, cov); });
        return;
    }
    const int cx0 = std::max(0, int(std::floor(clip_.x))), cx1 = std::min(w_, int(std::ceil(clip_.right())));
    raster_.for_each_span([&](int x0, int x1, int y, float cov) {
        int a = std::max(ox + x0, cx0), e = std::min(ox + x1, cx1);
        if (a < e) blend_span(a, e, oy + y, c, cov);
    });
}

void Canvas::fill_round_rect(const Rect& r, float radius, Color c) {
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) * 0.5f);
    if (deferring_) {  // the outline is built only if its tiles are drawn
        Cmd cmd(CmdKind::RoundRect);
        cmd.bounds = Rect{std::floor(r.x), std::floor(r.y), std::ceil(r.w) + 2, std::ceil(r.h) + 2}.intersect(clip_);
        if (cmd.bounds.empty() || c.a == 0) return;
        cmd.r = r, cmd.radius = radius, cmd.c = c;
        record(cmd);
        return;
    }
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

void Canvas::glow(const Rect& r, float radius, float spread, Color c) {
    if (!rec_ || c.a == 0 || spread < 1.f) return;
    if (r.inset(-spread).intersect(clip_).empty()) return;
    rec_->clip(clip_);
    rec_->glow(r, radius, spread, c);
}

bool Canvas::layer_begin(uint32_t id, uint64_t stamp, const Rect& r) {
    if (!rec_) return true;
    const bool have = rec_->layer_begin(id, stamp, r);
    if (!have) {  // its commands start from the canvas' whole area
        clip_stack_.push_back(clip_);
        clip_ = {0, 0, float(w_), float(h_)};
    }
    return !have;
}

void Canvas::layer_end() {
    if (!rec_) return;
    rec_->layer_end();
    if (!clip_stack_.empty()) {
        clip_ = clip_stack_.back();
        clip_stack_.pop_back();
    }
}

void Canvas::gradient(const Rect& r, float radius, Color from, Color to, bool vertical) {
    if (!rec_ || r.empty() || r.intersect(clip_).empty()) return;
    rec_->clip(clip_);
    rec_->gradient(r, std::clamp(radius, 0.f, std::min(r.w, r.h) * 0.5f), from, to, vertical);
}

void Canvas::fill_ellipse(const Rect& box, Color c) {
    if (c.a == 0 || box.w < 0.2f || box.h < 0.2f) return;
    if (rec_) {  // the GPU draws it: no mask to rasterize and keep
        if (box.inset(-1).intersect(clip_).empty()) return;
        rec_->clip(clip_);
        rec_->ellipse(box, c);
        return;
    }
    const float rx = box.w / 2, ry = box.h / 2, cx = box.cx(), cy = box.cy();
    const int n = std::clamp(int(std::sqrt(std::max(rx, ry)) * 4.f), 12, 96);
    Point pts[96];
    for (int k = 0; k < n; ++k) {  // clockwise on the screen
        float a = 6.2831853f * float(k) / float(n);
        pts[k] = {cx + rx * std::cos(a), cy + ry * std::sin(a)};
    }
    Path p;
    p.polygon(pts, n);
    fill_path(p, c);
}

void Canvas::fill_circle(float cx, float cy, float radius, Color c) {
    if (rec_) return fill_round_rect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c);
    if (deferring_) {
        Cmd cmd(CmdKind::Circle);
        cmd.bounds = Rect{std::floor(cx - radius), std::floor(cy - radius), std::ceil(radius * 2) + 2,
                          std::ceil(radius * 2) + 2}
                         .intersect(clip_);
        if (cmd.bounds.empty() || c.a == 0) return;
        cmd.x = cx, cmd.y = cy, cmd.radius = radius, cmd.c = c;
        record(cmd);
        return;
    }
    Path p;
    p.circle(cx, cy, radius);
    fill_path(p, c);
}

float Canvas::draw_text(Font& font, int px, float x, float baseline, std::string_view text, Color c) {
    if (deferring_) {
        float adv = font.measure(text, px);
        float asc = font.ascent(px) * 1.2f, desc = font.descent(px) * 1.3f, slack = float(px) * 0.3f + 2;
        Cmd cmd{CmdKind::Text};
        cmd.bounds = Rect{x - slack, baseline - asc - 2, adv + 2 * slack, asc + desc + 4}.intersect(clip_);
        if (cmd.bounds.empty() || c.a == 0) return adv;
        cmd.c = c;
        cmd.font = &font;
        cmd.px = px;
        cmd.x = x, cmd.y = baseline;
        cmd.index = uint32_t(text_.size());
        cmd.len = uint32_t(text.size());
        text_.append(text);
        record(cmd);
        return adv;
    }
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
        // Clip the glyph box once; the inner loop is then free of bounds checks.
        const int xa = std::max(0, cx0 - gx), xb = std::min(g.w, cx1 - gx);
        const uint32_t src_rb = uint32_t(c.r) << 16 | c.b, src_g = c.g;
        for (int y = 0; y < g.h; ++y) {
            int dy = gy + y;
            if (dy < cy0 || dy >= cy1) continue;
            const uint8_t* row = &g.alpha[size_t(y) * g.w];
            uint32_t* d = px_ + size_t(dy) * size_t(w_) + gx;
            for (int x = xa; x < xb; ++x) {
                unsigned cov = row[x];
                if (cov == 0) continue;
                unsigned a = c.a == 255 ? cov : (cov * c.a + 127) / 255;
                if (a >= 255) {
                    d[x] = c.xrgb();
                    continue;
                }
                unsigned ia = 255 - a;
                uint32_t v = d[x];
                uint32_t rb = src_rb * a + 0x00800080u + (v & 0x00FF00FFu) * ia;
                rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
                uint32_t gg = src_g * a + 0x80u + ((v >> 8) & 0xFFu) * ia;
                gg = ((gg + (gg >> 8)) >> 8) & 0xFFu;
                d[x] = 0xFF000000u | rb | gg << 8;
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

// ------------------------------------------------------------------ partial redraw

namespace {

constexpr int kTile = 32;

// Eight bytes a step (shapes hash every point of their outline each frame).
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void word(uint64_t v) {
        h ^= v * 0x9E3779B97F4A7C15ull;
        h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full;
    }
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        while (n >= 8) {
            uint64_t v;
            std::memcpy(&v, b, 8);
            word(v);
            b += 8, n -= 8;
        }
        uint64_t tail = n;
        for (size_t i = 0; i < n; ++i) tail = tail << 8 | b[i];
        word(tail);
    }
    template <class T>
    void add(const T& v) {
        bytes(&v, sizeof v);
    }
    void rect(const Rect& r) {
        add(r.x), add(r.y), add(r.w), add(r.h);
    }
};

}  // namespace

void Canvas::set_partial(bool on) {
    partial_ = on && !rec_;
    full_ = true;
    prev_tiles_.clear();
}

void Canvas::begin_frame() {
    if (!partial_) return;
    deferring_ = true;
    cmds_.clear();
    paths_used_ = 0;
    text_.clear();
    ++serial_;
    // Whatever no drawing covers is black, as after a full clear.
    Rect saved = clip_;
    clip_ = {0, 0, float(w_), float(h_)};
    clear(Color{0, 0, 0, 255});
    clip_ = saved;
}

void Canvas::record(Cmd& c) {
    Hasher h;
    h.add(c.kind);
    h.rect(c.clip = clip_);
    h.add(c.c.r), h.add(c.c.g), h.add(c.c.b), h.add(c.c.a);
    switch (c.kind) {
        case CmdKind::Clear: break;
        case CmdKind::Rect: h.rect(c.r); break;
        case CmdKind::Path:
            for (const auto& contour : paths_[c.index].contours()) {
                h.add(contour.size());
                if (!contour.empty()) h.bytes(contour.data(), contour.size() * sizeof(Point));
            }
            break;
        case CmdKind::Pixels:
            // Another process's pixels may change under the same pointer.
            h.add(c.src), h.add(c.w), h.add(c.h), h.add(c.stride), h.rect(c.r), h.add(serial_);
            break;
        case CmdKind::Text:
            h.add(c.font), h.add(c.px), h.add(c.x), h.add(c.y);
            h.bytes(text_.data() + c.index, c.len);
            break;
        case CmdKind::RoundRect: h.rect(c.r), h.add(c.radius); break;
        case CmdKind::Image: h.add(c.image->id), h.rect(c.r), h.add(c.radius), h.add(c.alpha), h.rect(c.crop); break;
        case CmdKind::Circle: h.add(c.x), h.add(c.y), h.add(c.radius); break;
    }
    c.hash = h.h;
    cmds_.push_back(c);
}

void Canvas::replay(const Cmd& c) {
    switch (c.kind) {
        case CmdKind::Clear: clear_now(c.c); break;
        case CmdKind::Rect: fill_rect(c.r, c.c); break;
        case CmdKind::Path: fill_path(paths_[c.index], c.c); break;
        case CmdKind::Pixels: draw_pixels(c.src, c.w, c.h, c.stride, c.r); break;
        case CmdKind::Text:
            draw_text(*c.font, c.px, c.x, c.y, std::string_view(text_.data() + c.index, c.len), c.c);
            break;
        case CmdKind::RoundRect: fill_round_rect(c.r, c.radius, c.c); break;
        case CmdKind::Image: draw_image_now(*c.image, c.r, c.radius, c.alpha, c.crop); break;
        case CmdKind::Circle: fill_circle(c.x, c.y, c.radius, c.c); break;
    }
}

void Canvas::end_frame() {
    if (!deferring_) return;
    deferring_ = false;
    const int tw = (w_ + kTile - 1) / kTile, th = (h_ + kTile - 1) / kTile;
    tiles_.assign(size_t(tw) * size_t(th), 0x9E3779B97F4A7C15ull);
    for (const Cmd& c : cmds_) {
        int tx0 = std::max(0, int(c.bounds.x) / kTile), ty0 = std::max(0, int(c.bounds.y) / kTile);
        int tx1 = std::min(tw - 1, int(std::ceil(c.bounds.right())) / kTile);
        int ty1 = std::min(th - 1, int(std::ceil(c.bounds.bottom())) / kTile);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                uint64_t& t = tiles_[size_t(ty) * size_t(tw) + size_t(tx)];
                t = (t ^ c.hash) * 0x100000001B3ull;  // the order of drawing counts
            }
    }
    const bool all = full_ || prev_tiles_.size() != tiles_.size();
    // Changed tiles as rectangles: runs in each row, joined with the same run of the row above.
    std::vector<Rect> rects;
    std::vector<size_t> open;  // rects that reach the row above
    for (int ty = 0; ty < th; ++ty) {
        std::vector<size_t> now;
        for (int tx = 0; tx < tw;) {
            size_t i = size_t(ty) * size_t(tw) + size_t(tx);
            if (!all && tiles_[i] == prev_tiles_[i]) {
                ++tx;
                continue;
            }
            int end = tx + 1;
            while (end < tw) {
                size_t j = size_t(ty) * size_t(tw) + size_t(end);
                if (!all && tiles_[j] == prev_tiles_[j]) break;
                ++end;
            }
            Rect r{float(tx * kTile), float(ty * kTile), float((end - tx) * kTile), float(kTile)};
            bool joined = false;
            for (size_t k : open)
                if (rects[k].x == r.x && rects[k].w == r.w && rects[k].bottom() == r.y) {
                    rects[k].h += r.h;
                    now.push_back(k);
                    joined = true;
                    break;
                }
            if (!joined) {
                now.push_back(rects.size());
                rects.push_back(r);
            }
            tx = end;
        }
        open = std::move(now);
    }
    const Rect whole{0, 0, float(w_), float(h_)};
    double area = 0;
    for (Rect& r : rects) {
        r = r.intersect(whole);
        if (r.empty()) continue;
        area += double(r.w) * double(r.h);
        replaying_ = true;
        for (const Cmd& c : cmds_) {
            if (c.bounds.intersect(r).empty()) continue;
            clip_ = c.clip.intersect(r);
            replay_clip_ = c.clip;
            replay(c);
        }
        replaying_ = false;
        damage_.push_back(r);
    }
    clip_ = whole;
    clip_stack_.clear();
    // FACET_CHECK_PARTIAL: the whole frame drawn again elsewhere must equal
    // what the partial redraw left (tests; slow).
    static const bool check = std::getenv("FACET_CHECK_PARTIAL") != nullptr;
    if (check) {
        std::vector<uint32_t> full(size_t(w_) * size_t(h_), 0xFF000000u);
        uint32_t* saved = px_;
        px_ = full.data();
        for (const Cmd& c : cmds_) {
            clip_ = c.clip;
            replay(c);
        }
        px_ = saved;
        clip_ = whole;
        // Anti-aliased edges cut by a tile may round one level differently: only more counts.
        size_t bad = 0, first = 0;
        int worst = 0;
        for (size_t i = 0; i < full.size(); ++i) {
            int d = 0;
            for (int sh = 0; sh < 24; sh += 8)
                d = std::max(d, std::abs(int((full[i] >> sh) & 0xFF) - int((px_[i] >> sh) & 0xFF)));
            worst = std::max(worst, d);
            if (d > 2 && !bad++) first = i;
        }
        stats_.mismatched = bad;
        if (bad)
            std::fprintf(stderr, "partial redraw: %zu pixels differ from a full redraw (by up to %d), first at %zu,%zu\n",
                         bad, worst, first % size_t(w_), first / size_t(w_));
        else if (worst > 0)
            std::fprintf(stderr, "partial redraw: rounding differences only (at most %d of 255)\n", worst);
    }
    stats_.commands = cmds_.size();
    stats_.area = w_ > 0 && h_ > 0 ? area / (double(w_) * double(h_)) : 0;
    prev_tiles_.swap(tiles_);
    full_ = false;
}

std::vector<Rect> Canvas::take_damage() {
    std::vector<Rect> d;
    d.swap(damage_);
    return d;
}

}  // namespace facet::gfx
