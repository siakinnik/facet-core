#include "ui/theme.h"

namespace facet::ui {

using gfx::Color;

Theme Theme::make(bool dark, float scale, Fonts* fonts) {
    Theme t;
    t.dark = dark;
    t.scale = scale;
    t.fonts = fonts;
    if (dark) {
        t.c = Palette{
            .bg = Color::hex(0x0E1116),
            .surface = Color::hex(0x191E26),
            .surface_pressed = Color::hex(0x252C37),
            .text = Color::hex(0xE7EBF0),
            .text_dim = Color::hex(0x8A94A2),
            .accent = Color::hex(0x7AA8F7),
            .on_accent = Color::hex(0x0B1220),
            .divider = Color::hex(0x232A34),
            .track = Color::hex(0x2F3744),
            .good = Color::hex(0x5CC98F),
            .warn = Color::hex(0xE9B44C),
            .bad = Color::hex(0xEF6A5E),
            .scrim = Color::hex(0x000000, 150),
        };
    } else {
        t.c = Palette{
            .bg = Color::hex(0xF1F3F6),
            .surface = Color::hex(0xFFFFFF),
            .surface_pressed = Color::hex(0xE6EAF0),
            .text = Color::hex(0x121720),
            .text_dim = Color::hex(0x5E6A7A),
            .accent = Color::hex(0x2E6BE6),
            .on_accent = Color::hex(0xFFFFFF),
            .divider = Color::hex(0xE4E8EE),
            .track = Color::hex(0xD3D9E2),
            .good = Color::hex(0x1C9A5F),
            .warn = Color::hex(0xB97D0C),
            .bad = Color::hex(0xD23F33),
            .scrim = Color::hex(0x0B1220, 110),
        };
    }
    return t;
}

}  // namespace facet::ui
