#include "ui/icons.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace facet::ui {

using gfx::Path;
using gfx::Point;
using gfx::Rect;

Icon icon_from_name(std::string_view name) {
    if (name == "clock") return Icon::Clock;
    if (name == "display") return Icon::Display;
    if (name == "camera") return Icon::Camera;
    if (name == "settings") return Icon::Settings;
    if (name == "warning") return Icon::Warning;
    if (name == "back") return Icon::Back;
    if (name == "chevron") return Icon::Chevron;
    if (name == "shift") return Icon::Shift;
    if (name == "backspace") return Icon::Backspace;
    if (name == "enter") return Icon::Enter;
    return Icon::Plugin;
}

// Icons are designed on a 24x24 grid and scaled into `box`.
void draw_icon(gfx::Canvas& c, Icon icon, const Rect& box, gfx::Color color) {
    float s = std::min(box.w, box.h) / 24.f;
    float ox = box.cx() - 12 * s, oy = box.cy() - 12 * s;
    auto P = [&](float x, float y) { return Point{ox + x * s, oy + y * s}; };
    float stroke = 2.f * s;
    Path p;

    switch (icon) {
        case Icon::None: return;
        case Icon::Clock: {
            p.ring(ox + 12 * s, oy + 12 * s, 9.5f * s, stroke);
            p.stroke_line(P(12, 12), P(12, 7), stroke);
            p.stroke_line(P(12, 12), P(15.5f, 14), stroke);
            break;
        }
        case Icon::Display: {
            Rect outer{ox + 2 * s, oy + 4 * s, 20 * s, 13 * s};
            p.round_rect(outer, 2.5f * s);
            p.round_rect(outer.inset(stroke), 1.f * s, true);
            p.stroke_line(P(12, 17), P(12, 20), stroke);
            p.stroke_line(P(8, 20.5f), P(16, 20.5f), stroke);
            p.circle(ox + 12 * s, oy + 10.5f * s, 2.2f * s);  // presence "eye"
            break;
        }
        case Icon::Camera: {
            Rect body{ox + 2 * s, oy + 6.5f * s, 20 * s, 13 * s};
            p.round_rect(body, 2.5f * s);
            p.round_rect(body.inset(stroke), 1.f * s, true);
            p.ring(ox + 12 * s, oy + 13 * s, 3.8f * s, stroke);
            p.round_rect({ox + 8 * s, oy + 4 * s, 8 * s, 3.5f * s}, 1.f * s);
            break;
        }
        case Icon::Settings: {
            float cx = ox + 12 * s, cy = oy + 12 * s;
            p.circle(cx, cy, 7.f * s);
            for (int i = 0; i < 8; ++i) {
                float a = float(i) * 3.14159265f / 4.f;
                float ca = std::cos(a), sa = std::sin(a);
                auto R = [&](float u, float v) {  // rotate tooth-local (u along radius) coords
                    return Point{cx + (u * ca - v * sa) * s, cy + (u * sa + v * ca) * s};
                };
                Point q[4] = {R(5.f, -1.8f), R(10.f, -1.5f), R(10.f, 1.5f), R(5.f, 1.8f)};
                p.polygon(q, 4);
            }
            p.circle(cx, cy, 3.f * s, true);
            break;
        }
        case Icon::Back: {
            p.stroke_line(P(15, 5), P(8, 12), stroke * 1.1f);
            p.stroke_line(P(8, 12), P(15, 19), stroke * 1.1f);
            break;
        }
        case Icon::Chevron: {
            p.stroke_line(P(9, 6), P(15, 12), stroke);
            p.stroke_line(P(15, 12), P(9, 18), stroke);
            break;
        }
        case Icon::Warning: {
            Point tri[3] = {P(12, 3), P(22, 20.5f), P(2, 20.5f)};
            p.polygon(tri, 3);
            Point hole[3] = {P(12, 7.5f), P(5.8f, 18.5f), P(18.2f, 18.5f)};
            p.polygon(hole, 3);  // opposite winding -> hole
            p.stroke_line(P(12, 10.5f), P(12, 14), stroke * 0.9f);
            p.circle(ox + 12 * s, oy + 16.6f * s, 1.1f * s);
            break;
        }
        case Icon::Shift: {  // outlined up arrow
            Point outer[7] = {P(12, 3), P(21, 12), P(16, 12), P(16, 20), P(8, 20), P(8, 12), P(3, 12)};
            p.polygon(outer, 7);
            Point inner[7] = {P(12, 6), P(6.5f, 10.5f), P(10, 10.5f), P(10, 18), P(14, 18), P(14, 10.5f), P(17.5f, 10.5f)};
            p.polygon(inner, 7);  // opposite winding -> hole
            break;
        }
        case Icon::Backspace: {  // tag pointing left with an x
            Point outer[5] = {P(8, 5), P(22, 5), P(22, 19), P(8, 19), P(1.5f, 12)};
            p.polygon(outer, 5);
            Point inner[5] = {P(8.9f, 7), P(4.2f, 12), P(8.9f, 17), P(20, 17), P(20, 7)};
            p.polygon(inner, 5);
            p.stroke_line(P(11.5f, 9), P(17, 15), stroke * 0.9f);
            p.stroke_line(P(17, 9), P(11.5f, 15), stroke * 0.9f);
            break;
        }
        case Icon::Enter: {  // return arrow
            p.stroke_line(P(19, 5), P(19, 14), stroke);
            p.stroke_line(P(19, 14), P(6, 14), stroke);
            p.stroke_line(P(6, 14), P(10, 10), stroke);
            p.stroke_line(P(6, 14), P(10, 18), stroke);
            break;
        }
        case Icon::Plugin: {
            Rect r{ox + 3 * s, oy + 3 * s, 18 * s, 18 * s};
            p.round_rect(r, 4 * s);
            p.round_rect(r.inset(stroke), 2.5f * s, true);
            p.circle(ox + 12 * s, oy + 12 * s, 3 * s);
            break;
        }
    }
    c.fill_path(p, color);
}

namespace {

// A thick arc as one polygon: outer edge forward, inner edge back.
void arc_band(Path& p, float cx, float cy, float r, float width, float a0, float a1) {
    std::vector<Point> pts;
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
        float a = a0 + (a1 - a0) * float(i) / n;
        pts.push_back({cx + std::cos(a) * (r + width / 2), cy + std::sin(a) * (r + width / 2)});
    }
    for (int i = n; i >= 0; --i) {
        float a = a0 + (a1 - a0) * float(i) / n;
        pts.push_back({cx + std::cos(a) * (r - width / 2), cy + std::sin(a) * (r - width / 2)});
    }
    p.polygon(pts.data(), int(pts.size()));
}

}  // namespace

void draw_net_icon(gfx::Canvas& c, NetIcon icon, int bars, const Rect& box, gfx::Color on, gfx::Color off) {
    float s = std::min(box.w, box.h) / 24.f;
    float ox = box.cx() - 12 * s, oy = box.cy() - 12 * s;
    float stroke = 2.2f * s;
    const float pi = 3.14159265f;

    if (icon == NetIcon::Ethernet) {
        // Network port: outlined box with three contacts.
        Path p;
        Rect r{ox + 3 * s, oy + 5 * s, 18 * s, 14 * s};
        p.round_rect(r, 2 * s);
        p.round_rect(r.inset(stroke), 1 * s, true);
        for (int i = 0; i < 3; ++i) p.rect({ox + (7.5f + 3.5f * float(i)) * s, oy + 8 * s, 2 * s, 4 * s});
        p.rect({ox + 9 * s, oy + 14 * s, 6 * s, 2.5f * s});
        c.fill_path(p, on);
        return;
    }

    // Wi-Fi: dot + three arcs opening upwards from (12, 19).
    float cx = ox + 12 * s, cy = oy + 19 * s;
    int lit = icon == NetIcon::Offline ? 0 : std::clamp(bars, 0, 3);
    Path dot;
    dot.circle(cx, cy - 0.5f * s, 1.8f * s);
    c.fill_path(dot, icon == NetIcon::Offline ? off : on);
    for (int i = 0; i < 3; ++i) {
        Path arc;
        arc_band(arc, cx, cy, (5.5f + 4.5f * float(i)) * s, stroke, -pi * 0.75f, -pi * 0.25f);
        c.fill_path(arc, i < lit ? on : off);
    }
    if (icon == NetIcon::Offline) {
        Path slash;
        slash.stroke_line({ox + 4 * s, oy + 4 * s}, {ox + 20 * s, oy + 20 * s}, stroke);
        c.fill_path(slash, on);
    }
}

}  // namespace facet::ui
