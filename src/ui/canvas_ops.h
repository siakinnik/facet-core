// Renders a plugin `canvas` widget: a list of draw operations in dp with theme
// colour tokens and touch targets (see sdk/include/facet/plugin.h, Canvas).
#pragma once

#include <string>
#include <string_view>

#include "facet/json.h"
#include "ui/context.h"

namespace facet::ui {

// Resolves a colour: theme token ("accent", "text", ...) or "#RRGGBB[AA]".
// Unknown values fall back to `fallback`.
gfx::Color color_from(const Theme& theme, std::string_view value, gfx::Color fallback);

// Draws `node` ({"height": dp, "ops": [...]}) as a full-width block in the
// current screen. Returns the `hit` id tapped this frame, or "".
std::string draw_canvas(Context& ui, std::string_view id, const Json& node);

// Draws `ops` inside `box` (e.g. a keyboard overlay). Returns the tapped hit id.
std::string draw_ops(Context& ui, std::string_view id, const Json& ops, const gfx::Rect& box);

}  // namespace facet::ui
