// Anti-aliased polygon rasterizer (signed-area accumulation). Shared by
// glyph rendering and vector shapes. Fill rule: non-zero with |winding|
// clamped to 1, so same-direction contours union and reversed ones cut holes.
#pragma once

#include <cmath>
#include <vector>

#include "gfx/types.h"

namespace facet::gfx {

class Rasterizer {
public:
    void reset(int w, int h);
    void line(Point a, Point b);

    int width() const { return w_; }
    int height() const { return h_; }

    // Calls f(x, y, coverage 0..1) for every covered pixel, row by row.
    template <class F>
    void for_each(F&& f) const {
        for (int y = 0; y < h_; ++y) {
            const float* row = &acc_[size_t(y) * stride_];
            float sum = 0;
            for (int x = 0; x < w_; ++x) {
                sum += row[x];
                float c = std::fabs(sum);
                if (c > 0.0015f) f(x, y, c > 1.f ? 1.f : c);
            }
        }
    }

private:
    void line_clipped(Point a, Point b);

    int w_ = 0, h_ = 0, stride_ = 0;
    std::vector<float> acc_;
};

// A path is a set of closed polygons, already flattened to pixel space.
class Path {
public:
    void move_to(float x, float y);
    void line_to(float x, float y);
    void quad_to(float cx, float cy, float x, float y);
    void close();

    // Shapes. `reverse` flips winding to cut a hole out of a same-direction shape.
    void rect(const Rect& r, bool reverse = false);
    void round_rect(const Rect& r, float radius, bool reverse = false);
    void circle(float cx, float cy, float radius, bool reverse = false);
    void ring(float cx, float cy, float radius, float thickness);
    // Thick line with round caps.
    void stroke_line(Point a, Point b, float width);
    void polygon(const Point* pts, int n);

    void transform(float dx, float dy);  // translate in place

    bool empty() const { return contours_.empty(); }
    const std::vector<std::vector<Point>>& contours() const { return contours_; }
    Rect bounds() const;

private:
    void add_arc(float cx, float cy, float r, float a0, float a1, std::vector<Point>& out) const;

    std::vector<std::vector<Point>> contours_;
    Point pen_;
};

void rasterize(const Path& path, Rasterizer& r, float dx, float dy);

}  // namespace facet::gfx
