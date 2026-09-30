// Plugin-side SDK: message loop over stdin/stdout (protocol v1, see
// docs/ARCHITECTURE.md) and a builder for declarative screens.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "facet/i18n.h"
#include "facet/json.h"

namespace facet::sdk {

constexpr int kApiVersion = 1;

// Custom drawing for a `canvas` widget: a list of draw operations the core
// renders with its own rasterizer. Coordinates and sizes are in dp relative to
// the canvas' top-left corner; the canvas is as wide as the content column
// (Plugin::content_width()). Colours are theme tokens ("bg", "surface",
// "surface_pressed", "text", "text_dim", "accent", "on_accent", "divider",
// "track", "good", "warn", "bad") or "#RRGGBB" / "#RRGGBBAA"; tokens follow
// the dark/light theme automatically.
//
// Shapes with a `hit` id are touch targets: a tap sends event(canvas id,
// hit id). While a finger is on them the core draws `pressed` (or a derived
// colour) at once, without a round trip to the plugin.
class Canvas {
public:
    Canvas& rect(float x, float y, float w, float h, std::string color, std::string hit = {},
                 std::string pressed = {});
    Canvas& rrect(float x, float y, float w, float h, float radius, std::string color, std::string hit = {},
                  std::string pressed = {});
    Canvas& circle(float cx, float cy, float r, std::string color, std::string hit = {}, std::string pressed = {});
    Canvas& ring(float cx, float cy, float r, float width, std::string color);
    Canvas& line(float x1, float y1, float x2, float y2, float width, std::string color);
    // Text vertically centred in the box; align: "start", "center", "end";
    // font: "regular", "medium", "light". Too long text is ellipsized.
    Canvas& text(float x, float y, float w, float h, std::string text, float size, std::string color,
                 std::string align = "start", std::string font = "regular");
    // Built-in icon by name (see manifest tile icons), `size` dp square.
    Canvas& icon(float x, float y, float size, std::string name, std::string color);

    const Json& ops() const { return ops_; }

private:
    Json& add(const char* op);
    Json ops_ = Json::array();
};

// Builds the UI tree a plugin sends to the core. Widget set mirrors ui::Context.
class Screen {
public:
    explicit Screen(std::string title);

    Screen& section(std::string title);
    Screen& info(std::string label, std::string value, std::string tone = "normal");
    Screen& note(std::string text);
    Screen& toggle(std::string id, std::string label, bool value);
    Screen& select(std::string id, std::string label, std::vector<std::string> options, int value);
    Screen& stepper(std::string id, std::string label, int value, int min, int max, int step,
                    std::string unit = {});
    Screen& time(std::string id, std::string label, int minutes, int step = 15);
    Screen& level(std::string label, double value, std::string text = {});
    Screen& button(std::string id, std::string label, std::string style = "normal");
    // Free drawing area, full content width, `height` dp tall.
    Screen& canvas(std::string id, float height, const Canvas& canvas);

    const Json& json() const { return root_; }

private:
    Json& add(const char* type);
    Json root_;
};

class Plugin {
public:
    Plugin(std::string id, std::string version);

    // Callbacks. All run on the thread that called run().
    std::function<void(const Json& hello)> on_hello;
    std::function<void(const std::string& id, const Json& value)> on_event;
    std::function<void(bool visible)> on_visible;
    // Called after the UI language changed (catalog already switched).
    std::function<void(const std::string& lang)> on_locale;
    // Called when the content width changed (e.g. another screen size).
    std::function<void(int content_width)> on_layout;
    std::function<void()> on_activity;
    std::function<void()> on_tick;
    std::function<void()> on_shutdown;

    const std::string& data_dir() const { return data_dir_; }
    // Width of the content column in dp: the width of canvas widgets.
    int content_width() const { return content_width_; }

    // Register translation tables here; the language follows the core.
    i18n::Catalog& catalog() { return catalog_; }
    std::string tr(std::string_view key) const { return catalog_.tr(key); }
    std::string tr(std::string_view key, const std::vector<std::string>& args) const { return catalog_.tr(key, args); }
    std::string render(const i18n::Text& t) const { return catalog_.render(t); }
    bool visible() const { return visible_; }

    // Senders skip messages identical to the previously sent value.
    void set_ui(const Screen& screen);
    void set_tile(const std::string& subtitle);
    void request_display(bool on);
    void send(const Json& msg);

    // Runs until the core sends shutdown or closes stdin. Returns exit code.
    int run(int tick_ms = 250);
    void quit() { running_ = false; }

    static void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

private:
    void handle(const Json& msg);

    void set_locale(const std::string& lang);

    std::string id_, version_, data_dir_;
    i18n::Catalog catalog_;
    bool visible_ = false;
    int content_width_ = 440;
    bool running_ = true;
    Json last_ui_;
    std::string last_tile_;
    int last_display_ = -1;
};

}  // namespace facet::sdk
