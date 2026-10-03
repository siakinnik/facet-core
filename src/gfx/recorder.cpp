#include "gfx/recorder.h"

#include <algorithm>
#include <cmath>

#include "gfx/font.h"

namespace facet::gfx {

namespace {
uint32_t pack(Color c) { return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | uint32_t(c.a) << 24; }

// FNV-1a over the bytes of a value.
void hash_bytes(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
}

constexpr uint64_t kPathKey = 1ull << 63;  // glyph keys never set it
}  // namespace

void Recorder::begin_frame(bool lost) {
    buf_.clear();
    clip_sent_ = false;
    if (lost) {
        pages_.clear();
        entries_.clear();
        carry_.clear();
    } else if (!sent_) {
        // The last frame was replaced before it reached the helper: its
        // uploads come along, later frames use what they put in the atlas.
        buf_ = carry_;
    } else {
        carry_.clear();
    }
    sent_ = false;
    // A full atlas starts over between frames (never inside one: the
    // regions this frame uses must stay valid until it is drawn).
    if (pages_.size() >= size_t(ops::kMaxAtlasPages) - 2) reset_atlas();
}

void Recorder::keep(const void* record) {
    const auto* h = static_cast<const ops::Header*>(record);
    const auto* b = static_cast<const uint8_t*>(record);
    carry_.insert(carry_.end(), b, b + h->size);
}

void Recorder::reset_atlas() {
    pages_.clear();
    entries_.clear();
    keep(add(ops::kAtlasReset, sizeof(ops::Header), 0));
}

void* Recorder::add(uint32_t type, size_t fixed, size_t payload) {
    size_t size = (fixed + payload + 3) & ~size_t(3);
    size_t at = buf_.size();
    buf_.resize(at + size);
    auto* h = reinterpret_cast<ops::Header*>(buf_.data() + at);
    h->type = type;
    h->size = uint32_t(size);
    return buf_.data() + at;
}

void Recorder::clip(const Rect& r) {
    if (clip_sent_ && r.x == clip_.x && r.y == clip_.y && r.w == clip_.w && r.h == clip_.h) return;
    clip_ = r;
    clip_sent_ = true;
    auto* o = static_cast<ops::Clip*>(add(ops::kClip, sizeof(ops::Clip), 0));
    o->x = r.x, o->y = r.y, o->w = r.w, o->h_ = r.h;
}

void Recorder::rect(const Rect& r, float radius, Color c) {
    auto* o = static_cast<ops::Rect*>(add(ops::kRect, sizeof(ops::Rect), 0));
    o->x = r.x, o->y = r.y, o->w = r.w, o->h_ = r.h, o->radius = radius;
    o->color = pack(c);
}

bool Recorder::place(uint64_t key, const uint8_t* alpha, int w, int h, Entry& out) {
    if (w > ops::kAtlasSize - 2 || h > ops::kAtlasSize - 2) return false;
    int pw = w + 1, ph = h + 1;  // a pixel of space keeps neighbours apart
    for (size_t pi = 0; pi <= pages_.size(); ++pi) {
        if (pi == pages_.size()) {
            if (pages_.size() >= size_t(ops::kMaxAtlasPages)) return false;
            pages_.emplace_back();
        }
        Page& pg = pages_[pi];
        Shelf* best = nullptr;
        for (auto& s : pg.shelves)
            if (s.h >= ph && s.h <= ph + ph / 2 + 4 && s.x + pw <= ops::kAtlasSize && (!best || s.h < best->h)) best = &s;
        if (!best && pg.bottom + ph <= ops::kAtlasSize) {
            pg.shelves.push_back({pg.bottom, ph, 0});
            pg.bottom += ph;
            best = &pg.shelves.back();
        }
        if (!best) continue;
        out = {uint32_t(pi), best->x, best->y, w, h};
        best->x += pw;
        auto* o = static_cast<ops::Upload*>(add(ops::kUpload, sizeof(ops::Upload), size_t(w) * size_t(h)));
        o->page = out.page, o->u = out.u, o->v = out.v, o->w = w, o->h_ = h;
        std::memcpy(o + 1, alpha, size_t(w) * size_t(h));
        keep(o);
        entries_[key] = out;
        return true;
    }
    return false;
}

void Recorder::mask(const Entry& e, int x, int y, Color c) {
    auto* o = static_cast<ops::Mask*>(add(ops::kMask, sizeof(ops::Mask), 0));
    o->x = x, o->y = y, o->page = e.page, o->u = e.u, o->v = e.v, o->w = e.w, o->h_ = e.h;
    o->color = pack(c);
}

void Recorder::inline_mask(const uint8_t* alpha, int w, int h, int x, int y, Color c) {
    auto* o = static_cast<ops::Mask*>(add(ops::kMask, sizeof(ops::Mask), size_t(w) * size_t(h)));
    o->x = x, o->y = y, o->page = ops::kInlinePage, o->u = 0, o->v = 0, o->w = w, o->h_ = h;
    o->color = pack(c);
    std::memcpy(o + 1, alpha, size_t(w) * size_t(h));
}

void Recorder::glyph(Font& font, uint32_t codepoint, int px, int x, int y, Color c) {
    const Glyph& g = font.glyph(codepoint, px);
    if (g.w <= 0 || g.h <= 0) return;
    uint64_t key = uint64_t(font.id() & 0x7FFF) << 48 | uint64_t(px & 0xFFFF) << 32 | codepoint;
    auto it = entries_.find(key);
    Entry e;
    if (it != entries_.end()) e = it->second;
    else if (!place(key, g.alpha.data(), g.w, g.h, e)) return inline_mask(g.alpha.data(), g.w, g.h, x, y, c);
    mask(e, x, y, c);
}

void Recorder::path(const Path& p, int ox, int oy, int w, int h, Color c) {
    // The same shape at the same sub-pixel offset rasterizes the same: the key
    // is its outline relative to the mask's corner.
    uint64_t key = 1469598103934665603ull;
    hash_bytes(key, &w, sizeof w);
    hash_bytes(key, &h, sizeof h);
    for (const auto& contour : p.contours()) {
        uint32_t n = uint32_t(contour.size());
        hash_bytes(key, &n, sizeof n);
        for (const Point& pt : contour) {
            float rel[2] = {pt.x - float(ox), pt.y - float(oy)};
            hash_bytes(key, rel, sizeof rel);
        }
    }
    key |= kPathKey;
    auto it = entries_.find(key);
    if (it != entries_.end()) return mask(it->second, ox, oy, c);
    raster_.reset(w, h);
    rasterize(p, raster_, -float(ox), -float(oy));
    scratch_.assign(size_t(w) * size_t(h), 0);
    raster_.for_each_span([&](int x0, int x1, int y, float cov) {
        std::memset(scratch_.data() + size_t(y) * size_t(w) + size_t(x0), int(cov * 255.f + 0.5f), size_t(x1 - x0));
    });
    Entry e;
    if (place(key, scratch_.data(), w, h, e)) mask(e, ox, oy, c);
    else inline_mask(scratch_.data(), w, h, ox, oy, c);
}

void Recorder::surface(uint32_t layer) {
    auto* o = static_cast<ops::Surface*>(add(ops::kSurface, sizeof(ops::Surface), 0));
    o->layer = layer;
}

void Recorder::image(const uint32_t* src, int w, int h, int stride, const Rect& dst) {
    auto* o = static_cast<ops::Image*>(add(ops::kImage, sizeof(ops::Image), size_t(w) * size_t(h) * 4));
    o->w = w, o->h_ = h, o->x = dst.x, o->y = dst.y, o->dw = dst.w, o->dh = dst.h;
    auto* out = reinterpret_cast<uint32_t*>(o + 1);
    for (int y = 0; y < h; ++y) std::memcpy(out + size_t(y) * size_t(w), src + size_t(y) * size_t(stride), size_t(w) * 4);
}

}  // namespace facet::gfx
