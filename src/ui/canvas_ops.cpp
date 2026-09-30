#include "ui/canvas_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "ui/icons.h"

namespace facet::ui {

using gfx::Color;
using gfx::Rect;

namespace {

constexpr float kMaxHeightDp = 4000;  // guards against absurd sizes from plugins
constexpr size_t kMaxOps = 2000;
constexpr size_t kMaxPoints = 1024;  // per poly/polyline op

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Pressed feedback when the plugin did not specify a colour: nudge towards
// the text colour, which reads as "lighter" in dark and "darker" in light.
Color pressed_of(const Theme& t, Color c) { return gfx::mix(c, t.c.text, 0.12f); }

FontRole font_of(const std::string& name) {
    if (name == "medium") return FontRole::Medium;
    if (name == "light") return FontRole::Light;
    return FontRole::Regular;
}

gfx::Align align_of(const std::string& name) {
    if (name == "center") return gfx::Align::Center;
    if (name == "end") return gfx::Align::End;
    return gfx::Align::Start;
}

// Clockwise in screen space, like Path::circle(), so shapes in one path add up
// instead of cancelling each other out.
void make_clockwise(std::vector<gfx::Point>& pts) {
    double area = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const gfx::Point& a = pts[i];
        const gfx::Point& b = pts[(i + 1) % pts.size()];
        area += double(a.x) * b.y - double(b.x) * a.y;
    }
    if (area < 0) std::reverse(pts.begin(), pts.end());
}

// A thick arc band from a0 to a1 (radians, a0 < a1) with round ends.
void arc_band(gfx::Path& p, float cx, float cy, float r, float width, float a0, float a1) {
    std::vector<gfx::Point> pts;
    int n = std::clamp(int((a1 - a0) * std::sqrt(std::max(r, 1.f)) * 1.5f), 2, 256);
    for (int i = 0; i <= n; ++i) {
        float a = a0 + (a1 - a0) * float(i) / float(n);
        pts.push_back({cx + std::cos(a) * (r + width / 2), cy + std::sin(a) * (r + width / 2)});
    }
    for (int i = n; i >= 0; --i) {
        float a = a0 + (a1 - a0) * float(i) / float(n);
        pts.push_back({cx + std::cos(a) * (r - width / 2), cy + std::sin(a) * (r - width / 2)});
    }
    make_clockwise(pts);
    p.polygon(pts.data(), int(pts.size()));
    p.circle(cx + std::cos(a0) * r, cy + std::sin(a0) * r, width / 2);
    p.circle(cx + std::cos(a1) * r, cy + std::sin(a1) * r, width / 2);
}

}  // namespace

Color color_from(const Theme& theme, std::string_view v, Color fallback) {
    const Palette& c = theme.c;
    if (v == "bg") return c.bg;
    if (v == "surface") return c.surface;
    if (v == "surface_pressed") return c.surface_pressed;
    if (v == "text") return c.text;
    if (v == "text_dim") return c.text_dim;
    if (v == "accent") return c.accent;
    if (v == "on_accent") return c.on_accent;
    if (v == "divider") return c.divider;
    if (v == "track") return c.track;
    if (v == "good") return c.good;
    if (v == "warn") return c.warn;
    if (v == "bad") return c.bad;
    if (!v.empty() && v[0] == '#' && (v.size() == 7 || v.size() == 9)) {
        uint8_t b[4] = {0, 0, 0, 255};
        for (size_t i = 0; i + 1 < v.size(); i += 2) {
            int hi = hex_digit(v[i + 1]), lo = i + 2 < v.size() ? hex_digit(v[i + 2]) : -1;
            if (hi < 0 || lo < 0) return fallback;
            b[i / 2] = uint8_t(hi * 16 + lo);
        }
        return Color{b[0], b[1], b[2], b[3]};
    }
    return fallback;
}

std::string draw_canvas(Context& ui, std::string_view id, const Json& node) {
    float height = std::clamp(float(node["height"].as_number(0)), 0.f, kMaxHeightDp);
    return draw_ops(ui, id, node["ops"], ui.block(height));
}

std::string draw_ops(Context& ui, std::string_view id, const Json& ops_json, const Rect& box) {
    const Theme& t = ui.theme();
    gfx::Canvas& cv = ui.canvas();
    auto X = [&](const Json& j, const char* k) { return box.x + t.dp(float(j[k].as_number())); };
    auto Y = [&](const Json& j, const char* k) { return box.y + t.dp(float(j[k].as_number())); };
    auto D = [&](const Json& j, const char* k) { return t.dp(float(j[k].as_number())); };
    auto points = [&](const Json& j) {
        std::vector<gfx::Point> pts;
        const auto& v = j["pts"].items();
        for (size_t k = 0; k + 1 < v.size() && pts.size() < kMaxPoints; k += 2)
            pts.push_back({box.x + t.dp(float(v[k].as_number())), box.y + t.dp(float(v[k + 1].as_number()))});
        return pts;
    };

    std::string tapped;
    cv.push_clip(box);
    const auto& ops = ops_json.items();
    for (size_t i = 0; i < ops.size() && i < kMaxOps; ++i) {
        const Json& op = ops[i];
        const std::string& kind = op["op"].str();
        Color color = color_from(t, op["color"].str(), t.c.text);

        // Touch target: shapes with a `hit` id.
        Rect bounds;
        if (kind == "rect" || kind == "rrect") bounds = {X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")};
        else if (kind == "circle") {
            float r = D(op, "r");
            bounds = {X(op, "cx") - r, Y(op, "cy") - r, 2 * r, 2 * r};
        }
        const std::string& hit = op["hit"].str();
        if (!hit.empty() && !bounds.empty()) {
            Context::Press pr = ui.press(std::string(id) + "\x1f" + hit, bounds);
            if (pr.held) color = color_from(t, op["pressed"].str(), pressed_of(t, color));
            if (pr.clicked && tapped.empty()) tapped = hit;
        }

        if (kind == "rect") {
            cv.fill_rect(bounds, color);
        } else if (kind == "rrect") {
            cv.fill_round_rect(bounds, D(op, "r"), color);
        } else if (kind == "circle") {
            cv.fill_circle(bounds.cx(), bounds.cy(), bounds.w / 2, color);
        } else if (kind == "ring") {
            gfx::Path p;
            p.ring(X(op, "cx"), Y(op, "cy"), D(op, "r"), std::max(1.f, D(op, "width")));
            cv.fill_path(p, color);
        } else if (kind == "line") {
            gfx::Path p;
            p.stroke_line({X(op, "x1"), Y(op, "y1")}, {X(op, "x2"), Y(op, "y2")}, std::max(1.f, D(op, "width")));
            cv.fill_path(p, color);
        } else if (kind == "arc") {
            // Angles in degrees, 0 = 12 o'clock, clockwise.
            float sweep = std::clamp(float(op["sweep"].as_number()), -360.f, 360.f);
            if (std::fabs(sweep) >= 0.1f) {
                float a0 = (float(op["start"].as_number()) - 90.f) * 3.14159265f / 180.f;
                float a1 = a0 + sweep * 3.14159265f / 180.f;
                gfx::Path p;
                arc_band(p, X(op, "cx"), Y(op, "cy"), D(op, "r"), std::max(1.f, D(op, "width")), std::min(a0, a1),
                         std::max(a0, a1));
                cv.fill_path(p, color);
            }
        } else if (kind == "poly") {
            std::vector<gfx::Point> pts = points(op);
            if (pts.size() >= 3) {
                make_clockwise(pts);
                gfx::Path p;
                p.polygon(pts.data(), int(pts.size()));
                cv.fill_path(p, color);
            }
        } else if (kind == "polyline") {
            std::vector<gfx::Point> pts = points(op);
            float w = std::max(1.f, D(op, "width"));
            gfx::Path p;
            for (size_t k = 1; k < pts.size(); ++k) p.stroke_line(pts[k - 1], pts[k], w);
            cv.fill_path(p, color);
        } else if (kind == "text") {
            Rect tb{X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")};
            float size = std::clamp(float(op["size"].as_number(Theme::kBody)), 6.f, 400.f);
            FontRole role = font_of(op["font"].str());
            ui.text(role, size, tb, ui.ellipsize(role, size, op["text"].str(), tb.w + 1), color,
                    align_of(op["align"].str()));
        } else if (kind == "icon") {
            float s = D(op, "size");
            draw_icon(cv, icon_from_name(op["name"].str()), {X(op, "x"), Y(op, "y"), s, s}, color);
        }
    }
    cv.pop_clip();
    return tapped;
}

}  // namespace facet::ui
