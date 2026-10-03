#include "gfx/font.h"

#include <cmath>
#include <fstream>
#include <iterator>

#include "gfx/utf8.h"

namespace facet::gfx {

namespace {
uint32_t tag(const char* t) {
    return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 |
           uint8_t(t[3]);
}
}  // namespace

bool Font::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (data_.size() < 12) return false;
    path_ = path;
    static uint32_t loads = 0;
    id_ = ++loads;
    cache_.clear();

    uint32_t base = 0;
    if (u32(0) == tag("ttcf")) base = u32(12);  // first face of a collection

    uint32_t head = 0, hhea = 0, maxp = 0, os2 = 0;
    int num_tables = u16(base + 4);
    for (int i = 0; i < num_tables; ++i) {
        uint32_t rec = base + 12 + 16 * uint32_t(i);
        uint32_t t = u32(rec), off = u32(rec + 8);
        if (t == tag("cmap")) cmap_ = off;
        else if (t == tag("glyf")) glyf_ = off;
        else if (t == tag("loca")) loca_ = off;
        else if (t == tag("hmtx")) hmtx_ = off;
        else if (t == tag("head")) head = off;
        else if (t == tag("hhea")) hhea = off;
        else if (t == tag("maxp")) maxp = off;
        else if (t == tag("OS/2")) os2 = off;
    }
    if (!cmap_ || !glyf_ || !loca_ || !hmtx_ || !head || !hhea || !maxp) return false;  // CFF fonts unsupported

    units_per_em_ = std::max<float>(16, u16(head + 18));
    loca_long_ = i16(head + 50);
    num_glyphs_ = u16(maxp + 4);
    ascent_ = i16(hhea + 4);
    descent_ = i16(hhea + 6);
    num_hmetrics_ = u16(hhea + 34);
    cap_height_ = units_per_em_ * 0.7f;
    if (os2 && u16(os2) >= 2) cap_height_ = i16(os2 + 88);

    // Pick the best Unicode cmap subtable: full repertoire (format 12) first.
    uint32_t best = 0;
    int best_rank = 0;
    int n = u16(cmap_ + 2);
    for (int i = 0; i < n; ++i) {
        uint32_t rec = cmap_ + 4 + 8 * uint32_t(i);
        uint16_t platform = u16(rec), encoding = u16(rec + 2);
        uint32_t sub = cmap_ + u32(rec + 4);
        uint16_t format = u16(sub);
        int rank = 0;
        if (format == 12 && (platform == 3 || platform == 0)) rank = 3;
        else if (format == 4 && platform == 3 && encoding == 1) rank = 2;
        else if (format == 4 && platform == 0) rank = 1;
        if (rank > best_rank) {
            best_rank = rank;
            best = sub;
        }
    }
    if (!best) return false;
    cmap_ = best;
    cmap_format_ = u16(best);
    return true;
}

uint16_t Font::glyph_index(uint32_t cp) const {
    if (cmap_format_ == 4) {
        if (cp > 0xFFFF) return 0;
        uint32_t seg_x2 = u16(cmap_ + 6);
        uint32_t ends = cmap_ + 14;
        uint32_t starts = ends + seg_x2 + 2;
        uint32_t deltas = starts + seg_x2;
        uint32_t ranges = deltas + seg_x2;
        // Binary search over end codes.
        uint32_t lo = 0, hi = seg_x2 / 2;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (u16(ends + 2 * mid) < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= seg_x2 / 2) return 0;
        uint32_t start = u16(starts + 2 * lo);
        if (cp < start) return 0;
        uint16_t delta = u16(deltas + 2 * lo);
        uint16_t range = u16(ranges + 2 * lo);
        if (range == 0) return uint16_t(cp + delta);
        uint32_t addr = ranges + 2 * lo + range + 2 * (cp - start);
        uint16_t g = u16(addr);
        return g ? uint16_t(g + delta) : 0;
    }
    if (cmap_format_ == 12) {
        uint32_t groups = u32(cmap_ + 12);
        uint32_t lo = 0, hi = groups;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t g = cmap_ + 16 + 12 * mid;
            if (cp < u32(g)) hi = mid;
            else if (cp > u32(g + 4)) lo = mid + 1;
            else return uint16_t(u32(g + 8) + (cp - u32(g)));
        }
    }
    return 0;
}

float Font::advance_units(uint16_t gid) const {
    if (num_hmetrics_ == 0) return 0;
    if (gid >= num_hmetrics_) gid = uint16_t(num_hmetrics_ - 1);
    return u16(hmtx_ + 4 * uint32_t(gid));
}

bool Font::glyph_offset(uint16_t gid, uint32_t& off, uint32_t& len) const {
    if (gid >= num_glyphs_) return false;
    uint32_t a, b;
    if (loca_long_) {
        a = u32(loca_ + 4 * uint32_t(gid));
        b = u32(loca_ + 4 * uint32_t(gid) + 4);
    } else {
        a = u16(loca_ + 2 * uint32_t(gid)) * 2u;
        b = u16(loca_ + 2 * uint32_t(gid) + 2) * 2u;
    }
    if (b <= a) return false;  // empty glyph (space)
    off = glyf_ + a;
    len = b - a;
    return off + len <= data_.size();
}

void Font::outline(uint16_t gid, const Matrix& m, Path& out, int depth) const {
    uint32_t off, len;
    if (depth > 8 || !glyph_offset(gid, off, len)) return;
    int16_t num_contours = i16(off);

    auto xf = [&m](float x, float y) { return Point{m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f}; };

    if (num_contours >= 0) {
        std::vector<uint16_t> ends(static_cast<size_t>(num_contours));
        for (int i = 0; i < num_contours; ++i) ends[size_t(i)] = u16(off + 10 + 2 * uint32_t(i));
        if (ends.empty()) return;
        size_t num_points = size_t(ends.back()) + 1;
        uint32_t p = off + 10 + 2 * uint32_t(num_contours);
        p += 2 + u16(p);  // skip instructions

        std::vector<uint8_t> flags(num_points);
        for (size_t i = 0; i < num_points;) {
            uint8_t fl = u8(p++);
            flags[i++] = fl;
            if (fl & 8) {
                uint8_t rep = u8(p++);
                while (rep-- && i < num_points) flags[i++] = fl;
            }
        }
        std::vector<Point> pts(num_points);
        int v = 0;
        for (size_t i = 0; i < num_points; ++i) {
            uint8_t fl = flags[i];
            if (fl & 2) v += (fl & 16) ? u8(p) : -int(u8(p)), p += 1;
            else if (!(fl & 16)) v += i16(p), p += 2;
            pts[i].x = float(v);
        }
        v = 0;
        for (size_t i = 0; i < num_points; ++i) {
            uint8_t fl = flags[i];
            if (fl & 4) v += (fl & 32) ? u8(p) : -int(u8(p)), p += 1;
            else if (!(fl & 32)) v += i16(p), p += 2;
            pts[i].y = float(v);
        }

        size_t start = 0;
        for (int c = 0; c < num_contours; ++c) {
            size_t end = ends[size_t(c)];
            if (end < start || end >= num_points) break;
            size_t n = end - start + 1;
            auto on = [&](size_t k) { return (flags[start + k % n] & 1) != 0; };
            auto pt = [&](size_t k) { return pts[start + k % n]; };

            // Start from an on-curve point, or the midpoint of two off-curve ones.
            size_t first = 0;
            while (first < n && !on(first)) ++first;
            Point startp;
            if (first == n) {
                Point a = pt(0), b = pt(1);
                startp = {(a.x + b.x) / 2, (a.y + b.y) / 2};
                first = 0;
            } else {
                startp = pt(first);
            }
            Point s = xf(startp.x, startp.y);
            out.move_to(s.x, s.y);
            bool have_ctrl = false;
            Point ctrl;
            for (size_t k = 1; k <= n; ++k) {
                size_t idx = first + k;
                Point cur = pt(idx);
                if (on(idx)) {
                    Point q = xf(cur.x, cur.y);
                    if (have_ctrl) {
                        Point cc = xf(ctrl.x, ctrl.y);
                        out.quad_to(cc.x, cc.y, q.x, q.y);
                    } else {
                        out.line_to(q.x, q.y);
                    }
                    have_ctrl = false;
                } else {
                    if (have_ctrl) {
                        Point mid{(ctrl.x + cur.x) / 2, (ctrl.y + cur.y) / 2};
                        Point cc = xf(ctrl.x, ctrl.y), q = xf(mid.x, mid.y);
                        out.quad_to(cc.x, cc.y, q.x, q.y);
                    }
                    ctrl = cur;
                    have_ctrl = true;
                }
            }
            if (have_ctrl) {
                Point cc = xf(ctrl.x, ctrl.y);
                out.quad_to(cc.x, cc.y, s.x, s.y);
            }
            start = end + 1;
        }
        return;
    }

    // Composite glyph.
    uint32_t p = off + 10;
    while (true) {
        uint16_t fl = u16(p), child = u16(p + 2);
        p += 4;
        float dx = 0, dy = 0;
        if (fl & 1) {
            if (fl & 2) dx = i16(p), dy = i16(p + 2);
            p += 4;
        } else {
            if (fl & 2) dx = int8_t(u8(p)), dy = int8_t(u8(p + 1));
            p += 2;
        }
        auto f2dot14 = [this](uint32_t o) { return i16(o) / 16384.f; };
        Matrix cm;
        if (fl & 8) {
            cm.a = cm.d = f2dot14(p);
            p += 2;
        } else if (fl & 0x40) {
            cm.a = f2dot14(p);
            cm.d = f2dot14(p + 2);
            p += 4;
        } else if (fl & 0x80) {
            cm.a = f2dot14(p);
            cm.b = f2dot14(p + 2);
            cm.c = f2dot14(p + 4);
            cm.d = f2dot14(p + 6);
            p += 8;
        }
        cm.e = dx;
        cm.f = dy;
        // Compose: parent ∘ child.
        Matrix r;
        r.a = m.a * cm.a + m.c * cm.b;
        r.b = m.b * cm.a + m.d * cm.b;
        r.c = m.a * cm.c + m.c * cm.d;
        r.d = m.b * cm.c + m.d * cm.d;
        r.e = m.a * cm.e + m.c * cm.f + m.e;
        r.f = m.b * cm.e + m.d * cm.f + m.f;
        outline(child, r, out, depth + 1);
        if (!(fl & 0x20)) break;
    }
}

const Glyph& Font::glyph(uint32_t cp, int px) {
    uint64_t key = uint64_t(cp) << 16 | uint64_t(px & 0xFFFF);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 4096) cache_.clear();

    Glyph& g = cache_[key];
    uint16_t gid = glyph_index(cp);
    if (gid == 0 && cp != ' ' && cp != 0xFFFD) gid = glyph_index(0xFFFD);
    float s = scale(px);
    g.advance = advance_units(gid) * s;

    // Outline in pixel space with y pointing down (baseline at y=0).
    Matrix m;
    m.a = s;
    m.d = -s;
    Path path;
    outline(gid, m, path, 0);
    if (path.empty()) return g;

    Rect b = path.bounds();
    int x0 = int(std::floor(b.x)), y0 = int(std::floor(b.y));
    int x1 = int(std::ceil(b.right())), y1 = int(std::ceil(b.bottom()));
    g.left = x0;
    g.top = -y0;
    g.w = x1 - x0 + 1;
    g.h = y1 - y0 + 1;
    raster_.reset(g.w, g.h);
    rasterize(path, raster_, -float(x0), -float(y0));
    g.alpha.assign(size_t(g.w) * size_t(g.h), 0);
    raster_.for_each([&](int x, int y, float c) { g.alpha[size_t(y) * g.w + x] = uint8_t(c * 255.f + 0.5f); });
    return g;
}

float Font::measure(std::string_view text, int px) {
    float w = 0;
    size_t i = 0;
    while (i < text.size()) w += glyph(utf8_next(text, i), px).advance;
    return w;
}

}  // namespace facet::gfx
