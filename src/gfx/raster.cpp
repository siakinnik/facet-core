#include "gfx/raster.h"

#include <algorithm>

namespace facet::gfx {

namespace {
constexpr float kPi = 3.14159265358979f;
}

void Rasterizer::reset(int w, int h) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    stride_ = w_ + 2;
    acc_.assign(size_t(stride_) * size_t(h_), 0.f);
}

// Splits the segment at x=0 and x=w so each piece can be clamped horizontally
// without changing the covered area inside the buffer.
void Rasterizer::line(Point a, Point b) {
    if (a.y == b.y || h_ == 0) return;
    float xs[2] = {0.f, float(w_)};
    Point pts[4] = {a};
    int n = 1;
    float ts[2];
    int nt = 0;
    for (float cx : xs) {
        if ((a.x < cx) != (b.x < cx) && a.x != b.x) {
            float t = (cx - a.x) / (b.x - a.x);
            if (t > 0.f && t < 1.f) ts[nt++] = t;
        }
    }
    if (nt == 2 && ts[0] > ts[1]) std::swap(ts[0], ts[1]);
    for (int i = 0; i < nt; ++i) pts[n++] = {a.x + (b.x - a.x) * ts[i], a.y + (b.y - a.y) * ts[i]};
    pts[n++] = b;
    for (int i = 0; i + 1 < n; ++i) {
        Point p = pts[i], q = pts[i + 1];
        p.x = std::clamp(p.x, 0.f, float(w_));
        q.x = std::clamp(q.x, 0.f, float(w_));
        line_clipped(p, q);
    }
}

void Rasterizer::line_clipped(Point p0, Point p1) {
    if (p0.y == p1.y) return;
    float dir = 1.f;
    if (p0.y > p1.y) {
        dir = -1.f;
        std::swap(p0, p1);
    }
    float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    float x = p0.x;
    if (p0.y < 0) x -= p0.y * dxdy;
    int y_start = std::max(0, int(p0.y));
    int y_end = std::min(h_, int(std::ceil(p1.y)));
    const int max_x = w_ + 1;

    for (int y = y_start; y < y_end; ++y) {
        float* row = &acc_[size_t(y) * stride_];
        float dy = std::min(float(y + 1), p1.y) - std::max(float(y), p0.y);
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float x0 = std::min(x, xnext), x1 = std::max(x, xnext);
        float x0floor = std::floor(x0);
        int x0i = int(x0floor);
        float x1ceil = std::ceil(x1);
        int x1i = int(x1ceil);
        x0i = std::clamp(x0i, 0, max_x);
        x1i = std::clamp(x1i, 0, max_x);

        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (x + xnext) - x0floor;
            row[x0i] += d - d * xmf;
            if (x0i + 1 <= max_x) row[x0i + 1] += d * xmf;
        } else {
            float s = 1.f / (x1 - x0);
            float x0f = x0 - x0floor;
            float a0 = 0.5f * s * (1.f - x0f) * (1.f - x0f);
            float x1f = x1 - x1ceil + 1.f;
            float am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; ++xi) row[xi] += d * s;
                float a2 = a1 + float(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

// ---------------------------------------------------------------- Path

void Path::move_to(float x, float y) {
    contours_.emplace_back();
    contours_.back().push_back({x, y});
    pen_ = {x, y};
}

void Path::line_to(float x, float y) {
    if (contours_.empty()) move_to(pen_.x, pen_.y);
    contours_.back().push_back({x, y});
    pen_ = {x, y};
}

void Path::quad_to(float cx, float cy, float x, float y) {
    Point p0 = pen_;
    float ddx = p0.x - 2 * cx + x, ddy = p0.y - 2 * cy + y;
    float dd = std::sqrt(ddx * ddx + ddy * ddy);
    int n = std::clamp(int(std::sqrt(dd * 2.f)) + 1, 1, 64);
    for (int i = 1; i <= n; ++i) {
        float t = float(i) / n, mt = 1 - t;
        line_to(mt * mt * p0.x + 2 * mt * t * cx + t * t * x, mt * mt * p0.y + 2 * mt * t * cy + t * t * y);
    }
}

void Path::close() {
    if (!contours_.empty() && !contours_.back().empty()) pen_ = contours_.back().front();
}

void Path::polygon(const Point* pts, int n) {
    if (n < 3) return;
    contours_.emplace_back(pts, pts + n);
}

void Path::rect(const Rect& r, bool reverse) {
    Point p[4] = {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
    if (reverse) std::reverse(p, p + 4);
    polygon(p, 4);
}

void Path::add_arc(float cx, float cy, float r, float a0, float a1, std::vector<Point>& out) const {
    int n = std::clamp(int(std::fabs(a1 - a0) * std::sqrt(std::max(r, 1.f)) * 1.2f), 2, 128);
    for (int i = 0; i <= n; ++i) {
        float a = a0 + (a1 - a0) * float(i) / n;
        out.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r});
    }
}

void Path::round_rect(const Rect& r, float radius, bool reverse) {
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) * 0.5f);
    if (radius < 0.5f) {
        rect(r, reverse);
        return;
    }
    std::vector<Point> pts;
    float x0 = r.x + radius, x1 = r.right() - radius, y0 = r.y + radius, y1 = r.bottom() - radius;
    add_arc(x1, y0, radius, -kPi / 2, 0, pts);
    add_arc(x1, y1, radius, 0, kPi / 2, pts);
    add_arc(x0, y1, radius, kPi / 2, kPi, pts);
    add_arc(x0, y0, radius, kPi, kPi * 1.5f, pts);
    if (reverse) std::reverse(pts.begin(), pts.end());
    contours_.push_back(std::move(pts));
}

void Path::circle(float cx, float cy, float radius, bool reverse) {
    std::vector<Point> pts;
    add_arc(cx, cy, radius, 0, 2 * kPi, pts);
    pts.pop_back();
    if (reverse) std::reverse(pts.begin(), pts.end());
    contours_.push_back(std::move(pts));
}

void Path::ring(float cx, float cy, float radius, float thickness) {
    circle(cx, cy, radius);
    circle(cx, cy, radius - thickness, true);
}

void Path::stroke_line(Point a, Point b, float width) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    float hw = width * 0.5f;
    circle(a.x, a.y, hw);
    circle(b.x, b.y, hw);
    if (len < 1e-3f) return;
    float nx = -dy / len * hw, ny = dx / len * hw;
    // Same orientation as circle(): clockwise in screen space.
    Point p[4] = {{a.x - nx, a.y - ny}, {b.x - nx, b.y - ny}, {b.x + nx, b.y + ny}, {a.x + nx, a.y + ny}};
    polygon(p, 4);
}

void Path::transform(float dx, float dy) {
    for (auto& c : contours_)
        for (auto& p : c) {
            p.x += dx;
            p.y += dy;
        }
}

Rect Path::bounds() const {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& c : contours_)
        for (const auto& p : c) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    if (x0 > x1) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

void rasterize(const Path& path, Rasterizer& r, float dx, float dy) {
    for (const auto& c : path.contours()) {
        size_t n = c.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; ++i) {
            const Point& a = c[i];
            const Point& b = c[(i + 1) % n];
            r.line({a.x + dx, a.y + dy}, {b.x + dx, b.y + dy});
        }
    }
}

}  // namespace facet::gfx
