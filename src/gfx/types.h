#pragma once

#include <algorithm>
#include <cstdint>

namespace facet::gfx {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    static constexpr Color hex(uint32_t rgb, uint8_t a = 255) {
        return Color{uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a};
    }
    constexpr Color alpha(float f) const {
        return Color{r, g, b, uint8_t(std::clamp(f, 0.f, 1.f) * a)};
    }
    constexpr uint32_t xrgb() const { return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b; }
};

inline Color mix(Color a, Color b, float t) {
    auto l = [t](uint8_t x, uint8_t y) { return uint8_t(x + (y - x) * t + 0.5f); };
    return Color{l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a)};
}

struct Point {
    float x = 0, y = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect intersect(const Rect& o) const {
        float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    bool empty() const { return w <= 0 || h <= 0; }
};

}  // namespace facet::gfx
