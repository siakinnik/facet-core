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

// A rounded rectangle blending from `a` to `b`: strips of the shape, each
// clipped out of the whole and filled with its own colour.
void draw_gradient(gfx::Canvas& cv, Rect r, float radius, Color a, Color b, bool vertical) {
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
    if (cv.recording()) {  // one quad, the GPU blends the colours
        cv.gradient(r, radius, a, b, vertical);
        return;
    }
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

// Light that fades with the distance from the centre, like (1 - distance)^2.2.
// The GPU gets one command and a shader (nothing to stack: every layer would
// cost a pass over a big area); the processor rings of falling opacity.
void draw_glow(gfx::Canvas& cv, float cx, float cy, float radius, Color c) {
    if (radius < 1.f) return;
    radius = std::min(radius, 3000.f);
    if (cv.recording()) {
        cv.glow({cx, cy, 0, 0}, 0, radius, c);
        return;
    }
    const int steps = 18;
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
    if (cv.recording()) {
        cv.glow(r, radius, spread, c);
        return;
    }
    const int steps = 14;
    for (int k = 0; k < steps; ++k) {
        float s_in = spread * float(k) / float(steps), s_out = spread * float(k + 1) / float(steps);
        float mid = (float(k) + 0.5f) / float(steps);
        gfx::Path p;
        p.round_rect(r.inset(-s_out), radius + s_out);
        p.round_rect(r.inset(-s_in), radius + s_in, true);
        cv.fill_path(p, c.alpha(std::pow(1.f - mid, 2.2f)));
    }
}

// ------------------------------------------------------------ compiled ops

enum class Kind : uint8_t { None, Rect, RRect, Circle, Ellipse, Ring, Line, Arc, Poly, Polyline, Grad, Glow, RGlow, Text, Icon };
enum class WaveKind : uint8_t { None, Sin, Tri, Saw, Pulse, Flicker };

// A number of an op: a plain one, or k + a * width + b * height (+ a wave).
struct CNum {
    bool set = false;
    WaveKind wave = WaveKind::None;
    double k = 0, w = 0, h = 0, lo = 0, hi = 1, period = 1, phase = 0;
};

struct CColor {
    enum Kind : uint8_t { Name, Card, Tone, Fixed } kind = Name;
    Color fixed;
    std::string name;
};

}  // namespace

struct CompiledOps {
    struct Op {
        Kind kind = Kind::None;
        CNum x, y, w, h, r, cx, cy, rx, ry, spread, width, size, sweep, start, alpha, x1, y1, x2, y2;
        CColor color, pressed, from, to;
        std::string hit, text, name;
        FontRole font = FontRole::Regular;
        gfx::Align align = gfx::Align::Start;
        bool vertical = true;
        std::vector<CNum> pts;
    };
    std::vector<Op> ops;
};

namespace {

WaveKind wave_of(const std::string& kind) {
    if (kind.empty()) return WaveKind::None;
    if (kind == "tri") return WaveKind::Tri;
    if (kind == "saw") return WaveKind::Saw;
    if (kind == "pulse") return WaveKind::Pulse;
    if (kind == "flicker") return WaveKind::Flicker;
    return WaveKind::Sin;
}

double wave01(WaveKind kind, double x) {
    double frac = x - std::floor(x);
    constexpr double kTwoPi = 6.283185307179586;
    switch (kind) {
        case WaveKind::Tri: return frac < 0.5 ? frac * 2 : 2 - frac * 2;
        case WaveKind::Saw: return frac;
        case WaveKind::Pulse: {
            double s = 0.5 - 0.5 * std::cos(kTwoPi * x);
            return s * s * s * s;
        }
        case WaveKind::Flicker: {  // six irregular values a period, blended into each other
            double y = x * 6;
            double i = std::floor(y), f = y - i;
            double sm = f * f * (3 - 2 * f);
            return hash01(int64_t(i)) * (1 - sm) + hash01(int64_t(i) + 1) * sm;
        }
        default: return 0.5 - 0.5 * std::cos(kTwoPi * x);  // "sin": starts at the bottom
    }
}

CNum compile_num(const Json& v) {
    CNum n;
    if (v.is_number()) {
        n.set = true;
        n.k = v.as_number();
    } else if (v.is_object()) {
        n.set = true;
        n.k = v["k"].as_number(0);
        n.w = v["w"].as_number(0);
        n.h = v["h"].as_number(0);
        n.wave = wave_of(v["wave"].str());
        if (n.wave != WaveKind::None) {
            n.period = std::max(0.2, v["period"].as_number(1));
            n.lo = v["min"].as_number(0);
            n.hi = v["max"].as_number(1);
            n.phase = v["phase"].as_number(0);
        }
    }
    return n;
}

CColor compile_color(Context& ui, const std::string& name) {
    CColor c;
    c.name = name;
    if (name == "card") c.kind = CColor::Card;
    else if (name == "tone") c.kind = CColor::Tone;
    else if (const Skin* sk = ui.skin()) {
        auto it = sk->custom.find(name);
        if (it != sk->custom.end()) {
            c.kind = CColor::Fixed;
            c.fixed = it->second;
        }
    }
    return c;
}

// What the numbers of one list of ops are measured in.
struct Eval {
    Context& ui;
    double w_units, h_units;  // the box, in op units
    double time;
    bool moving;

    double operator()(const CNum& n, double def = 0) {
        if (!n.set) return def;
        double x = n.k + n.w * w_units + n.h * h_units;
        if (n.wave != WaveKind::None) {
            x += n.lo + (n.hi - n.lo) * wave01(n.wave, time / n.period + n.phase);
            if (moving) ui.mark_animated();
        }
        return x;
    }
};

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

std::shared_ptr<CompiledOps> compile_ops(Context& ui, const Json& ops_json) {
    auto out = std::make_shared<CompiledOps>();
    const auto& ops = ops_json.items();
    out->ops.reserve(std::min(ops.size(), kMaxOps));
    for (size_t i = 0; i < ops.size() && i < kMaxOps; ++i) {
        const Json& j = ops[i];
        CompiledOps::Op op;
        const std::string& kind = j["op"].str();
        static const std::pair<const char*, Kind> kKinds[] = {
            {"rect", Kind::Rect},     {"rrect", Kind::RRect}, {"circle", Kind::Circle},     {"ellipse", Kind::Ellipse},
            {"ring", Kind::Ring},     {"line", Kind::Line},   {"arc", Kind::Arc},           {"poly", Kind::Poly},
            {"polyline", Kind::Polyline}, {"grad", Kind::Grad}, {"glow", Kind::Glow},       {"rglow", Kind::RGlow},
            {"text", Kind::Text},     {"icon", Kind::Icon}};
        for (const auto& [name, k] : kKinds)
            if (kind == name) op.kind = k;
        op.x = compile_num(j["x"]), op.y = compile_num(j["y"]), op.w = compile_num(j["w"]), op.h = compile_num(j["h"]);
        op.r = compile_num(j["r"]), op.cx = compile_num(j["cx"]), op.cy = compile_num(j["cy"]);
        op.rx = compile_num(j["rx"]), op.ry = compile_num(j["ry"]), op.spread = compile_num(j["spread"]);
        op.width = compile_num(j["width"]), op.size = compile_num(j["size"]), op.sweep = compile_num(j["sweep"]);
        op.start = compile_num(j["start"]);
        op.x1 = compile_num(j["x1"]), op.y1 = compile_num(j["y1"]), op.x2 = compile_num(j["x2"]), op.y2 = compile_num(j["y2"]);
        if (j.contains("alpha")) {
            op.alpha = compile_num(j["alpha"]);
            if (!op.alpha.set) op.alpha.k = 1;  // present, but no number: opaque
            op.alpha.set = true;
        }
        op.color = compile_color(ui, j["color"].str());
        op.pressed = compile_color(ui, j["pressed"].str());
        op.from = compile_color(ui, j["from"].str());
        op.to = compile_color(ui, j["to"].str());
        op.hit = j["hit"].str();
        op.text = j["text"].str();
        op.name = j["name"].str();
        op.font = font_of(j["font"].str());
        op.align = align_of(j["align"].str());
        op.vertical = j["dir"].str() != "h";
        if (op.kind == Kind::Poly || op.kind == Kind::Polyline) {
            const auto& v = j["pts"].items();
            for (size_t k = 0; k + 1 < v.size() && op.pts.size() < kMaxPoints * 2; ++k) op.pts.push_back(compile_num(v[k]));
        }
        out->ops.push_back(std::move(op));
    }
    return out;
}

std::string draw_compiled(Context& ui, std::string_view id, const CompiledOps& compiled, const Rect& box,
                          const OpsEnv& env) {
    const Theme& t = ui.theme();
    gfx::Canvas& cv = ui.canvas();
    const float unit = env.unit;
    Eval N{ui, double(box.w) / double(t.scale * unit), double(box.h) / double(t.scale * unit),
           env.skin ? ui.skin_time() : ui.time(), ui.animations()};
    auto X = [&](const CNum& n) { return box.x + t.dp(float(N(n)) * unit); };
    auto Y = [&](const CNum& n) { return box.y + t.dp(float(N(n)) * unit); };
    auto D = [&](const CNum& n) { return t.dp(float(N(n)) * unit); };
    auto points = [&](const std::vector<CNum>& v) {
        std::vector<gfx::Point> pts;
        for (size_t k = 0; k + 1 < v.size() && pts.size() < kMaxPoints; k += 2)
            pts.push_back({box.x + t.dp(float(N(v[k])) * unit), box.y + t.dp(float(N(v[k + 1])) * unit)});
        return pts;
    };
    const Skin* skin = ui.skin();
    auto colour = [&](const CColor& c, Color fallback) {
        switch (c.kind) {
            case CColor::Card:
                if (env.card) return *env.card;
                break;
            case CColor::Tone:
                if (env.tone) return *env.tone;
                break;
            case CColor::Fixed: return c.fixed;
            default: break;
        }
        // Not known when compiled (a theme chosen since): the same lookups as ever.
        if (skin) {
            auto it = skin->custom.find(c.name);
            if (it != skin->custom.end()) return it->second;
        }
        return color_from(t, c.name, fallback);
    };

    std::string tapped;
    cv.push_clip(env.clip.empty() ? box : env.clip);
    for (const auto& op : compiled.ops) {
        const Kind kind = op.kind;
        Color color = colour(op.color, t.c.text);
        if (op.alpha.set) color = color.alpha(std::clamp(float(N(op.alpha, 1)), 0.f, 1.f));

        // Touch target: shapes with a `hit` id.
        Rect bounds;
        if (kind == Kind::Rect || kind == Kind::RRect) bounds = {X(op.x), Y(op.y), D(op.w), D(op.h)};
        else if (kind == Kind::Circle) {
            float r = D(op.r);
            bounds = {X(op.cx) - r, Y(op.cy) - r, 2 * r, 2 * r};
        }
        if (env.touch && !op.hit.empty() && !bounds.empty()) {
            Context::Press pr = ui.press(std::string(id) + "\x1f" + op.hit, bounds);
            if (pr.held) color = colour(op.pressed, pressed_of(t, color));
            if (pr.clicked && tapped.empty()) tapped = op.hit;
        }

        switch (kind) {
            case Kind::Rect: cv.fill_rect(bounds, color); break;
            case Kind::RRect: cv.fill_round_rect(bounds, D(op.r), color); break;
            case Kind::Circle: cv.fill_circle(bounds.cx(), bounds.cy(), bounds.w / 2, color); break;
            case Kind::Ellipse: {
                float rx = D(op.rx), ry = D(op.ry), cx = X(op.cx), cy = Y(op.cy);
                if (rx > 0.1f && ry > 0.1f) cv.fill_ellipse({cx - rx, cy - ry, 2 * rx, 2 * ry}, color);
                break;
            }
            case Kind::Ring: {
                gfx::Path p;
                p.ring(X(op.cx), Y(op.cy), D(op.r), std::max(1.f, D(op.width)));
                cv.fill_path(p, color);
                break;
            }
            case Kind::Line: {
                gfx::Path p;
                p.stroke_line({X(op.x1), Y(op.y1)}, {X(op.x2), Y(op.y2)}, std::max(1.f, D(op.width)));
                cv.fill_path(p, color);
                break;
            }
            case Kind::Arc: {
                // Angles in degrees, 0 = 12 o'clock, clockwise.
                float sweep = std::clamp(float(N(op.sweep)), -360.f, 360.f);
                if (std::fabs(sweep) >= 0.1f) {
                    float a0 = (float(N(op.start)) - 90.f) * 3.14159265f / 180.f;
                    float a1 = a0 + sweep * 3.14159265f / 180.f;
                    gfx::Path p;
                    arc_band(p, X(op.cx), Y(op.cy), D(op.r), std::max(1.f, D(op.width)), std::min(a0, a1),
                             std::max(a0, a1));
                    cv.fill_path(p, color);
                }
                break;
            }
            case Kind::Poly: {
                std::vector<gfx::Point> pts = points(op.pts);
                if (pts.size() >= 3) {
                    make_clockwise(pts);
                    gfx::Path p;
                    p.polygon(pts.data(), int(pts.size()));
                    cv.fill_path(p, color);
                }
                break;
            }
            case Kind::Polyline: {
                std::vector<gfx::Point> pts = points(op.pts);
                float w = std::max(1.f, D(op.width));
                gfx::Path p;
                for (size_t k = 1; k < pts.size(); ++k) p.stroke_line(pts[k - 1], pts[k], w);
                cv.fill_path(p, color);
                break;
            }
            case Kind::Grad: {
                Color from = colour(op.from, t.c.surface), to = colour(op.to, t.c.bg);
                if (op.alpha.set) {
                    float a = std::clamp(float(N(op.alpha, 1)), 0.f, 1.f);
                    from = from.alpha(a);
                    to = to.alpha(a);
                }
                draw_gradient(cv, {X(op.x), Y(op.y), D(op.w), D(op.h)}, D(op.r), from, to, op.vertical);
                break;
            }
            case Kind::Glow: draw_glow(cv, X(op.cx), Y(op.cy), D(op.r), color); break;
            case Kind::RGlow: draw_rect_glow(cv, {X(op.x), Y(op.y), D(op.w), D(op.h)}, D(op.r), D(op.spread), color); break;
            case Kind::Text: {
                Rect tb{X(op.x), Y(op.y), D(op.w), D(op.h)};
                float size = std::clamp(float(N(op.size, Theme::kBody)) * unit, 6.f, 400.f);
                ui.text(op.font, size, tb, ui.ellipsize(op.font, size, op.text, tb.w + 1), color, op.align);
                break;
            }
            case Kind::Icon: {
                float s = D(op.size);
                draw_icon(cv, icon_from_name(op.name), {X(op.x), Y(op.y), s, s}, color);
                break;
            }
            default: break;
        }
    }
    cv.pop_clip();
    return tapped;
}

std::string draw_ops(Context& ui, std::string_view id, const Json& ops_json, const Rect& box, const OpsEnv& env) {
    return draw_compiled(ui, id, *compile_ops(ui, ops_json), box, env);
}

}  // namespace facet::ui
