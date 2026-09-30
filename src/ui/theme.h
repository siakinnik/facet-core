// Design tokens. Every widget takes its colours and sizes from here and
// nowhere else, which keeps built-in screens and plugins visually identical.
#pragma once

#include "gfx/font.h"
#include "gfx/types.h"

namespace facet::ui {

enum class FontRole { Regular, Medium, Light };

struct Fonts {
    gfx::Font regular, medium, light;
    gfx::Font& get(FontRole r) { return r == FontRole::Medium ? medium : r == FontRole::Light ? light : regular; }
};

struct Palette {
    gfx::Color bg, surface, surface_pressed, text, text_dim, accent, on_accent, divider, track, good, warn, bad,
        scrim;
};

struct Theme {
    bool dark = true;
    float scale = 1.f;
    Palette c;
    Fonts* fonts = nullptr;

    float dp(float v) const { return v * scale; }
    int px(float dp_size) const { return int(dp_size * scale + 0.5f); }

    // Metrics in dp.
    static constexpr float kGutter = 20;       // screen side padding
    static constexpr float kMaxContent = 760;  // content column max width
    static constexpr float kHeader = 72;
    static constexpr float kRow = 64;
    static constexpr float kRadius = 18;
    static constexpr float kGap = 12;
    static constexpr float kTouch = 48;  // minimum touch target

    // Font sizes in dp.
    static constexpr float kTitle = 26;
    static constexpr float kBody = 19;
    static constexpr float kSmall = 15;

    static Theme make(bool dark, float scale, Fonts* fonts);
};

}  // namespace facet::ui
