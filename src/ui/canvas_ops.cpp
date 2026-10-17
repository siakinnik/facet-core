#include "ui/canvas_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "ui/icons.h"
#include "ui/skin.h"

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


uint32_t hash32(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return n;
}

double hash01(int64_t i) { return double(hash32(uint32_t(i) * 2654435761U + 12345U) & 0xFFFFFF) / 16777216.0; }

// 0..1 along a wave at `x` periods.
double wave01(const std::string& kind, double x) {
    double frac = x - std::floor(x);
    constexpr double kTwoPi = 6.283185307179586;
    if (kind == "tri") return frac < 0.5 ? frac * 2 : 2 - frac * 2;
    if (kind == "saw") return frac;
    if (kind == "pulse") {
        double s = 0.5 - 0.5 * std::cos(kTwoPi * x);
        return s * s * s * s;
    }
    if (kind == "flicker") {  // six irregular values a period, blended into each other
        double y = x * 6;
        double i = std::floor(y), f = y - i;
        double sm = f * f * (3 - 2 * f);
        return hash01(int64_t(i)) * (1 - sm) + hash01(int64_t(i) + 1) * sm;
    }
    return 0.5 - 0.5 * std::cos(kTwoPi * x);  // "sin": starts at the bottom
}

// What the numbers of one list of ops are measured in.
struct Eval {
    Context& ui;
    double w_units, h_units;  // the box, in op units
    double time;
    bool moving;

    double operator()(const Json& v, double def = 0) {
        if (v.is_number()) return v.as_number();
        if (!v.is_object()) return def;
        double x = v["k"].as_number(0) + v["w"].as_number(0) * w_units + v["h"].as_number(0) * h_units;
        const std::string& kind = v["wave"].str();
        if (!kind.empty()) {
            double period = std::max(0.2, v["period"].as_number(1));
            double lo = v["min"].as_number(0), hi = v["max"].as_number(1);
            x += lo + (hi - lo) * wave01(kind, time / period + v["phase"].as_number(0));
            if (moving) ui.mark_animated();
        }
        return x;
    }
    double operator()(const Json& op, const char* key, double def = 0) { return (*this)(op[key], def); }
};

// A rounded rectangle blending from `a` to `b`: strips of the shape, each
// clipped out of the whole and filled with its own colour.
void draw_gradient(gfx::Canvas& cv, Rect r, float radius, Color a, Color b, bool vertical) {
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
    float extent = vertical ? r.h : r.w;
    int steps = std::clamp(int(extent / 3.f), 2, 80);
    for (int i = 0; i < steps; ++i) {
        float c0 = std::floor(extent * float(i) / float(steps));
        float c1 = i + 1 == steps ? extent : std::floor(extent * float(i + 1) / float(steps));
        if (c1 <= c0) continue;
        Rect strip = vertical ? Rect{r.x, r.y + c0, r.w, c1 - c0} : Rect{r.x + c0, r.y, c1 - c0, r.h};
        Color c = gfx::mix(a, b, (float(i) + 0.5f) / float(steps));
        cv.push_clip(strip);
        if (radius > 0.5f) cv.fill_round_rect(r, radius, c);
        else cv.fill_rect(strip, c);
        cv.pop_clip();
    }
}

// Opacity of each of `n` equal-colour layers, outermost first, so that the
// stack adds up to a glow of `a0` at its strongest falling off like
// (1 - distance)^2.2 towards the outer edge. Layer j is drawn over layers
// 0..j-1, so (1 - A_j) = (1 - a_j) * (1 - A_{j-1}).
void glow_layers(int n, float a0, float* out) {
    float before = 0;  // A_{j-1}: what the layers outside this one add up to
    for (int j = 0; j < n; ++j) {
        float f = (float(n - j) - 0.5f) / float(n);  // distance of the band, 0 at the centre
        float target = a0 * std::pow(1.f - f, 2.2f);
        out[j] = target <= before ? 0.f : 1.f - (1.f - target) / (1.f - before);
        before = std::max(before, target);
    }
}

// Light that fades with the distance from the centre. For the GPU (recording)
// a stack of circles, which it draws itself; masks of big rings would fill its
// atlas. For the processor rings of falling opacity: far less to paint.
void draw_glow(gfx::Canvas& cv, float cx, float cy, float radius, Color c) {
    if (radius < 1.f) return;
    radius = std::min(radius, 3000.f);
    const int steps = cv.recording() ? 16 : 18;
    if (cv.recording()) {
        float a[24];
        glow_layers(steps, float(c.a) / 255.f, a);
        for (int j = 0; j < steps; ++j) {
            if (a[j] <= 0.002f) continue;
            Color layer = c;
            layer.a = uint8_t(std::clamp(a[j] * 255.f + 0.5f, 1.f, 255.f));
            cv.fill_circle(cx, cy, radius * float(steps - j) / float(steps), layer);
        }
        return;
    }
    for (int k = 0; k < steps; ++k) {
        float r_in = radius * float(k) / float(steps), r_out = radius * float(k + 1) / float(steps);
        float mid = (float(k) + 0.5f) / float(steps);
        Color ring = c.alpha(std::pow(1.f - mid, 2.2f));
        if (k == 0) {
            cv.fill_circle(cx, cy, r_out + 0.4f, ring);
        } else {
            gfx::Path p;
            p.ring(cx, cy, (r_in + r_out) / 2, (r_out - r_in) + 0.8f);
            cv.fill_path(p, ring);
        }
    }
}

// The same around a rounded rectangle, strongest at its edge.
void draw_rect_glow(gfx::Canvas& cv, Rect r, float radius, float spread, Color c) {
    if (spread < 1.f) return;
    spread = std::min(spread, 1500.f);
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
    const int steps = 14;
    if (cv.recording()) {
        float a[steps];
        glow_layers(steps, float(c.a) / 255.f, a);
        for (int j = 0; j < steps; ++j) {
            if (a[j] <= 0.002f) continue;
            float grow = spread * float(steps - j) / float(steps);
            Color layer = c;
            layer.a = uint8_t(std::clamp(a[j] * 255.f + 0.5f, 1.f, 255.f));
            cv.fill_round_rect(r.inset(-grow), radius + grow, layer);
        }
        return;
    }
    for (int k = 0; k < steps; ++k) {
        float s_in = spread * float(k) / float(steps), s_out = spread * float(k + 1) / float(steps);
        float mid = (float(k) + 0.5f) / float(steps);
        gfx::Path p;
        p.round_rect(r.inset(-s_out), radius + s_out);
        p.round_rect(r.inset(-s_in), radius + s_in, true);
        cv.fill_path(p, c.alpha(std::pow(1.f - mid, 2.2f)));
    }
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

std::string draw_ops(Context& ui, std::string_view id, const Json& ops_json, const Rect& box, const OpsEnv& env) {
    const Theme& t = ui.theme();
    gfx::Canvas& cv = ui.canvas();
    const float unit = env.unit;
    Eval N{ui, double(box.w) / double(t.scale * unit), double(box.h) / double(t.scale * unit), ui.time(),
           ui.animations()};
    auto X = [&](const Json& j, const char* k) { return box.x + t.dp(float(N(j, k)) * unit); };
    auto Y = [&](const Json& j, const char* k) { return box.y + t.dp(float(N(j, k)) * unit); };
    auto D = [&](const Json& j, const char* k) { return t.dp(float(N(j, k)) * unit); };
    auto points = [&](const Json& j) {
        std::vector<gfx::Point> pts;
        const auto& v = j["pts"].items();
        for (size_t k = 0; k + 1 < v.size() && pts.size() < kMaxPoints; k += 2)
            pts.push_back({box.x + t.dp(float(N(v[k])) * unit), box.y + t.dp(float(N(v[k + 1])) * unit)});
        return pts;
    };
    auto colour = [&](const std::string& name, Color fallback) {
        if (env.names) {
            auto it = env.names->find(name);
            if (it != env.names->end()) return it->second;
        }
        if (const Skin* sk = ui.skin()) {
            auto it = sk->custom.find(name);
            if (it != sk->custom.end()) return it->second;
        }
        return color_from(t, name, fallback);
    };

    std::string tapped;
    cv.push_clip(env.clip.empty() ? box : env.clip);
    const auto& ops = ops_json.items();
    for (size_t i = 0; i < ops.size() && i < kMaxOps; ++i) {
        const Json& op = ops[i];
        const std::string& kind = op["op"].str();
        Color color = colour(op["color"].str(), t.c.text);
        if (op.contains("alpha")) color = color.alpha(std::clamp(float(N(op, "alpha", 1)), 0.f, 1.f));

        // Touch target: shapes with a `hit` id.
        Rect bounds;
        if (kind == "rect" || kind == "rrect") bounds = {X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")};
        else if (kind == "circle") {
            float r = D(op, "r");
            bounds = {X(op, "cx") - r, Y(op, "cy") - r, 2 * r, 2 * r};
        }
        const std::string& hit = op["hit"].str();
        if (env.touch && !hit.empty() && !bounds.empty()) {
            Context::Press pr = ui.press(std::string(id) + "\x1f" + hit, bounds);
            if (pr.held) color = colour(op["pressed"].str(), pressed_of(t, color));
            if (pr.clicked && tapped.empty()) tapped = hit;
        }

        if (kind == "rect") {
            cv.fill_rect(bounds, color);
        } else if (kind == "rrect") {
            cv.fill_round_rect(bounds, D(op, "r"), color);
        } else if (kind == "circle") {
            cv.fill_circle(bounds.cx(), bounds.cy(), bounds.w / 2, color);
        } else if (kind == "ellipse") {
            float rx = D(op, "rx"), ry = D(op, "ry"), cx = X(op, "cx"), cy = Y(op, "cy");
            if (rx > 0.1f && ry > 0.1f) {
                int n = std::clamp(int(std::sqrt(std::max(rx, ry)) * 4.f), 12, 96);
                std::vector<gfx::Point> pts;
                for (int k = 0; k < n; ++k) {
                    float a = 6.2831853f * float(k) / float(n);
                    pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
                }
                make_clockwise(pts);
                gfx::Path p;
                p.polygon(pts.data(), int(pts.size()));
                cv.fill_path(p, color);
            }
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
            float sweep = std::clamp(float(N(op, "sweep")), -360.f, 360.f);
            if (std::fabs(sweep) >= 0.1f) {
                float a0 = (float(N(op, "start")) - 90.f) * 3.14159265f / 180.f;
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
        } else if (kind == "grad") {
            Color from = colour(op["from"].str(), t.c.surface), to = colour(op["to"].str(), t.c.bg);
            if (op.contains("alpha")) {
                float a = std::clamp(float(N(op, "alpha", 1)), 0.f, 1.f);
                from = from.alpha(a);
                to = to.alpha(a);
            }
            draw_gradient(cv, {X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")}, D(op, "r"), from, to,
                          op["dir"].str() != "h");
        } else if (kind == "glow") {
            draw_glow(cv, X(op, "cx"), Y(op, "cy"), D(op, "r"), color);
        } else if (kind == "rglow") {
            draw_rect_glow(cv, {X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")}, D(op, "r"), D(op, "spread"), color);
        } else if (kind == "text") {
            Rect tb{X(op, "x"), Y(op, "y"), D(op, "w"), D(op, "h")};
            float size = std::clamp(float(N(op, "size", Theme::kBody)) * unit, 6.f, 400.f);
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
