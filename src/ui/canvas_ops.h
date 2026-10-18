// Renders a plugin `canvas` widget: a list of draw operations in dp with theme
// colour tokens and touch targets (see sdk/include/facet/plugin.h, Canvas).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "facet/json.h"
#include "ui/context.h"

namespace facet::ui {

// Resolves a colour: theme token ("accent", "text", ...) or "#RRGGBB[AA]".
// Unknown values fall back to `fallback`.
gfx::Color color_from(const Theme& theme, std::string_view value, gfx::Color fallback);

// Draws `node` ({"height": dp, "ops": [...]}) as a full-width block in the
// current screen. Returns the `hit` id tapped this frame, or "".
std::string draw_canvas(Context& ui, std::string_view id, const Json& node);

// How a list of ops is drawn.
struct OpsEnv {
    float unit = 1.f;  // dp per op unit (e.g. 32 dp / 24 for a tile icon drawn on a 24 x 24 grid)
    // Where drawing may reach (a glow around a tile goes past the tile); empty: the box.
    gfx::Rect clip;
    // A tile's colours, for the names "card" and "tone" (a theme's drawings).
    const gfx::Color* card = nullptr;
    const gfx::Color* tone = nullptr;
    bool touch = true;  // false: `hit` is ignored (a theme's drawings take no touches)
    bool skin = false;  // a theme's drawings: time moves in steps (Context::skin_time)
};

// A list of ops in a form that draws without parsing again (themes draw the same list every frame).
struct CompiledOps;
std::shared_ptr<CompiledOps> compile_ops(Context& ui, const Json& ops);
std::string draw_compiled(Context& ui, std::string_view id, const CompiledOps& ops, const gfx::Rect& box,
                          const OpsEnv& env);

// Draws `ops` inside `box` (e.g. a keyboard overlay). Returns the tapped hit id.
// A number in an op is dp (times `unit`), or an object: {"w": a, "h": b, "k": c}
// is a * width + b * height of the box + c; adding "wave": "sin" | "tri" |
// "saw" | "pulse" | "flicker" with "min", "max", "period" (s) and "phase"
// (0..1) makes it move with time (see sdk/include/facet/theme.h).
std::string draw_ops(Context& ui, std::string_view id, const Json& ops, const gfx::Rect& box, const OpsEnv& env);
inline std::string draw_ops(Context& ui, std::string_view id, const Json& ops, const gfx::Rect& box,
                            float unit = 1.f) {
    OpsEnv env;
    env.unit = unit;
    return draw_ops(ui, id, ops, box, env);
}

}  // namespace facet::ui
