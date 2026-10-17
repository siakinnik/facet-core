// Themes for Facet: a module ships a theme as a data file (JSON) that the
// core reads and draws; no code of the module runs in Facet for it. This
// header builds that file.
//
//   facet::sdk::ThemeDef t("halloween");
//   t.name("en", "Halloween").dark(true);
//   t.color("bg", "#0B0614").color("accent", "#FF8A1F");
//   t.screen("*").grad(0, 0, Num::width(), Num::height(), 0, "#0B0614", "#1C0B2B");
//   t.tile().grad(0, 0, Num::width(), Num::height(), 24, "#24163A", "#1A1028");
//   t.save("halloween.json");
//
// and the module's manifest.json (no "exec": the module is only data) lists it:
//
//   "themes": [{ "id": "halloween", "name": { "en": "Halloween" }, "file": "halloween.json" }]
//
// The user picks it in Settings > Theme. What a theme can do:
//  - colours: every token of the palette ("bg", "surface", "surface_pressed",
//    "text", "text_dim", "accent", "on_accent", "divider", "track", "good",
//    "warn", "bad", "scrim"), plus names of its own for use in its drawings;
//  - the corner radius of tiles and cards;
//  - drawings: under and over each screen ("*" is every screen, or "menu",
//    "dashboard", "settings", "graphics", "plugin", "apps", "appinfo",
//    "notifications"), on every tile of the menu and on every card of the
//    lists (settings, installed modules, ...). A tile's or card's "under"
//    drawing replaces the plain rounded rectangle; "over" is drawn after its
//    content. Drawings are for looking at: they take no touches. Dialogs, the
//    keyboard and permission prompts are drawn over everything, a theme cannot
//    cover them.
//
// Inside a drawing, coordinates are dp from the top left corner of the box the
// drawing is for (the screen, a tile, a card). A number is a Num: plain, or
// relative to the box (Num::width(0.5) is half of its width, Num::height(1,
// -30) its height minus 30 dp), or moving with time (Num::wave(...): glows that
// breathe, a flame that flickers, a bat that crosses the screen). Colours are
// tokens, "#RRGGBB" or "#RRGGBBAA"; in a tile "card" is the tile's colour (it
// lightens while pressed) and "tone" its accent.
//
// Moving parts cost: the screen is redrawn 15 times a second while one is
// visible. ThemeDef::animated(true) tells the core there are some, so that the
// user can switch animation off (everything then stands at the start of its
// wave).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "facet/json.h"

namespace facet::sdk {

enum class Wave {
    Sine,     // smooth: min, max, min ... (starts at min)
    Triangle, // straight up and down
    Saw,      // min to max, then jumps back (things that travel across the screen)
    Pulse,    // short bright pulses with long rests (blinking)
    Flicker,  // irregular, between min and max (candles, flames)
};

// A number of a drawing: dp, a part of the box's width or height, and/or a wave.
class Num {
public:
    Num(double dp = 0) : k_(dp) {}  // NOLINT: plain numbers convert on purpose
    Num(int dp) : k_(dp) {}         // NOLINT
    Num(float dp) : k_(dp) {}       // NOLINT
    // `fraction` of the box's width / height, plus `add` dp.
    static Num width(double fraction = 1, double add = 0);
    static Num height(double fraction = 1, double add = 0);
    // Between `min` and `max`, once through in `period_s` seconds; `phase`
    // (0..1) shifts it so that several things need not move together.
    static Num wave(Wave kind, double min, double max, double period_s, double phase = 0);
    // Sum; at most one of the two may be a wave.
    Num operator+(const Num& o) const;
    Num operator-(const Num& o) const;
    // Every part (box fractions, dp, the wave's reach) times `f`.
    Num operator*(double f) const;
    Json to_json() const;

private:
    double w_ = 0, h_ = 0, k_ = 0;
    bool wave_ = false;
    Wave kind_ = Wave::Sine;
    double min_ = 0, max_ = 1, period_ = 1, phase_ = 0;
};

// A list of drawing operations (shapes, text, icons) for one place of a theme.
// Drawn in the order added.
class Scene {
public:
    Scene& rect(Num x, Num y, Num w, Num h, std::string color);
    Scene& rrect(Num x, Num y, Num w, Num h, Num radius, std::string color);
    Scene& circle(Num cx, Num cy, Num r, std::string color);
    Scene& ellipse(Num cx, Num cy, Num rx, Num ry, std::string color);
    Scene& ring(Num cx, Num cy, Num r, Num width, std::string color);
    Scene& line(Num x1, Num y1, Num x2, Num y2, Num width, std::string color);
    // Part of a ring with round ends: degrees, 0 = 12 o'clock, clockwise.
    Scene& arc(Num cx, Num cy, Num r, Num width, Num start, Num sweep, std::string color);
    // Filled polygon / stroked open line through {x0, y0, x1, y1, ...} (at most 1024 numbers).
    Scene& poly(std::vector<Num> points, std::string color);
    Scene& polyline(std::vector<Num> points, Num width, std::string color);
    Scene& text(Num x, Num y, Num w, Num h, std::string text, Num size, std::string color,
                std::string align = "start", std::string font = "regular");
    Scene& icon(Num x, Num y, Num size, std::string name, std::string color);
    // A rounded rectangle blending from colour `from` to `to`, downwards
    // (vertical) or to the right.
    Scene& grad(Num x, Num y, Num w, Num h, Num radius, std::string from, std::string to, bool vertical = true);
    // A soft light: brightest at the centre, fading to nothing at `r`.
    Scene& glow(Num cx, Num cy, Num r, std::string color);
    // A soft light around a rounded rectangle (strongest at its edge, fading
    // out `spread` dp away); the rectangle itself is left to what is drawn on it.
    Scene& rglow(Num x, Num y, Num w, Num h, Num radius, Num spread, std::string color);
    // Opacity (0..1, also a Num) of the last shape added.
    Scene& alpha(Num a);

    const Json& ops() const { return ops_; }
    bool empty() const { return ops_.size() == 0; }

private:
    Json& add(const char* op);
    Json ops_ = Json::array();
};

enum class Layer { Under, Over };

// One theme: its colours, corner radius and drawings.
class ThemeDef {
public:
    // `id`: letters, digits, - and _ (the same as a module id).
    explicit ThemeDef(std::string id) : id_(std::move(id)) {}
    // The name in a language ("en" is the fallback).
    ThemeDef& name(const std::string& lang, const std::string& text);
    // The base palette to start from: dark (default) or light; colours given
    // with color() replace its entries.
    ThemeDef& dark(bool on);
    ThemeDef& color(const std::string& token, const std::string& value);
    // Corner radius of tiles and cards, dp.
    ThemeDef& radius(float dp);
    ThemeDef& animated(bool on);
    Scene& screen(const std::string& screen, Layer layer = Layer::Under);
    Scene& tile(Layer layer = Layer::Under);
    Scene& card(Layer layer = Layer::Under);

    const std::string& id() const { return id_; }
    // The file's content (format 1).
    Json to_json() const;
    bool save(const std::string& path, std::string* error = nullptr) const;

private:
    std::string id_;
    std::map<std::string, std::string> names_;
    bool dark_ = true, animated_ = false;
    float radius_ = 0;
    std::map<std::string, std::string> colors_;
    std::map<std::string, Scene> scenes_;  // "screen/<id>/under", "tile/over", ...
};

}  // namespace facet::sdk
