// Built-in vector icon set. Plugins reference icons by name in manifests.
#pragma once

#include <string_view>

#include "gfx/canvas.h"

namespace facet::ui {

enum class Icon { None, Clock, Display, Camera, Settings, Back, Plugin, Warning, Chevron };

Icon icon_from_name(std::string_view name);
void draw_icon(gfx::Canvas& c, Icon icon, const gfx::Rect& box, gfx::Color color);

enum class NetIcon { Offline, Ethernet, Wifi };
// Wi-Fi draws `bars` (0..3) arcs in `on` and the rest in `off`.
void draw_net_icon(gfx::Canvas& c, NetIcon icon, int bars, const gfx::Rect& box, gfx::Color on, gfx::Color off);

}  // namespace facet::ui
