#include "ui/canvas_ops.h"

#include <algorithm>
#include <cstdlib>

#include "ui/icons.h"

namespace facet::ui {

using gfx::Color;
using gfx::Rect;

namespace {

constexpr float kMaxHeightDp = 4000;  // guards against absurd sizes from plugins
constexpr size_t kMaxOps = 2000;

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
