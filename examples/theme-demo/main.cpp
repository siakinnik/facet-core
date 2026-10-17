// Example theme: "Aurora", a dark blue-green theme with a slow northern light
// over the screens, glowing tiles and cards. Writes aurora.json. A template for
// themes: see sdk/include/facet/theme.h for everything a theme can do.
//
//   theme-demo <output.json>
#include <cstdio>

#include "facet/theme.h"

using namespace facet::sdk;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.json>\n", argv[0]);
        return 2;
    }
    ThemeDef t("aurora");
    t.name("en", "Aurora").dark(true).animated(true).radius(20);
    t.color("bg", "#050B14")
        .color("surface", "#0E1B2A")
        .color("surface_pressed", "#17304A")
        .color("accent", "#4FE3C1")
        .color("on_accent", "#04120E")
        .color("divider", "#18293C")
        .color("track", "#25405C")
        .color("northern", "#3CF0B0");  // a name of the theme's own, used in its drawings below

    // Behind every screen: a deep gradient and a slow green light that breathes.
    Scene& sky = t.screen("*");
    sky.grad(0, 0, Num::width(), Num::height(), 0, "#050B14", "#0A1F2E");
    sky.glow(Num::width(0.3), Num::height(0, 40), 360, "northern")
        .alpha(Num::wave(Wave::Sine, 0.12, 0.3, 9.0));
    sky.glow(Num::width(0.75) + Num::wave(Wave::Sine, -40, 40, 14.0), Num::height(0, 70), 300, "#7A5CFF")
        .alpha(Num::wave(Wave::Sine, 0.1, 0.24, 11.0, 0.4));

    // Every tile: a glow in its colour, a dark glassy body, a thin bright edge on top.
    Scene& tile = t.tile();
    tile.rglow(0, 0, Num::width(), Num::height(), 20, 12, "tone").alpha(0.22);
    tile.grad(0, 0, Num::width(), Num::height(), 20, "#12263A", "#0A1522");
    tile.rrect(14, 0, Num::width(1, -28), 1.5, 1, "tone").alpha(0.7);
    tile.rrect(0, 0, Num::width(), Num::height(), 20, "card").alpha(0.3);  // lightens while pressed

    Scene& card = t.card();
    card.grad(0, 0, Num::width(), Num::height(), 20, "#102235", "#0A1522");
    card.rrect(16, 0, Num::width(1, -32), 1.5, 1, "northern").alpha(0.5);

    std::string error;
    if (!t.save(argv[1], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
