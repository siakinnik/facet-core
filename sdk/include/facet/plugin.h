// Plugin-side SDK: message loop over stdin/stdout (protocol v1, see
// docs/ARCHITECTURE.md) and a builder for declarative screens.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "facet/i18n.h"
#include "facet/json.h"

namespace facet::sdk {

// Protocol and manifest version. Plugins built for another API version are
// not started; the core shows them as incompatible until they are updated.
constexpr int kApiVersion = 3;
// Version of the SDK this plugin is built with ("0.3.0-alpha").
const char* sdk_version();

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
    // Part of a ring with round ends: angles in degrees, 0 = 12 o'clock,
    // clockwise (gauges, progress).
    Canvas& arc(float cx, float cy, float r, float width, float start, float sweep, std::string color);
    // Filled polygon and a stroked open line through points {x0, y0, x1, y1, ...}
    // (charts). At most 1024 points each.
    Canvas& poly(std::vector<float> points, std::string color);
    Canvas& polyline(std::vector<float> points, float width, std::string color);
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

class Plugin;

// Pixels drawn by the plugin itself (video, camera, remote screens, ...):
// XRGB8888 in shared memory, double-buffered. Draw into pixels() when
// ready(), then present(); the core shows it with Screen::surface() or full
// screen (Screen::fullscreen()) and sends touches in its pixel coordinates.
// Needs a core with surfaces (0.5+); older cores ignore it.
class Surface {
public:
    Surface() = default;
    ~Surface() { destroy(); }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Creates (or resizes) the surface. False if shared memory is unavailable.
    bool create(Plugin& plugin, const std::string& id, int width, int height);
    void destroy();
    bool valid() const { return map_ != nullptr; }
    // A buffer is free: the core has taken the last presented one.
    bool ready() const { return valid() && !waiting_; }
    uint32_t* pixels();  // the buffer to draw now
    int width() const { return w_; }
    int height() const { return h_; }
    int stride() const { return w_; }  // in pixels
    const std::string& id() const { return id_; }
    // Shows what was drawn; the next frame goes into the other buffer.
    void present();

private:
    friend class Plugin;
    Plugin* plugin_ = nullptr;
    std::string id_;
    int w_ = 0, h_ = 0, back_ = 0;
    bool waiting_ = false;
    void* map_ = nullptr;
    size_t size_ = 0;
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
    // A Surface, full content width; height 0 keeps its aspect ratio.
    Screen& surface(std::string id, float height = 0);
    // Shows the Surface over the whole screen instead of the widgets (the
    // user swipes down from the top edge to leave).
    Screen& fullscreen(std::string surface_id);
    // Text input. Tapping it opens the on-screen keyboard; every change is
    // sent as event(id, text), "Done" as submit(id, text). mode: "text" or
    // "number". `secure` masks the value and always uses the core's built-in
    // keyboard, so keyboard plugins never see what is typed (PINs, passwords).
    Screen& text_field(std::string id, std::string label, std::string value, std::string placeholder = {},
                       bool secure = false, std::string mode = "text", int max_length = 256);

    const Json& json() const { return root_; }

private:
    Json& add(const char* type);
    Json root_;
};

// A notification posted to the core (permission "notifications"). Posting
// again with the same id replaces it.
struct Notification {
    std::string id;
    std::string title, body;
    // "message", "call" (full-screen incoming call with Accept / Decline,
    // rings until answered or cancelled), "alarm", "status" (silent, no banner).
    std::string kind = "message";
    std::string priority = "normal";  // "low", "normal", "high" (high and calls wake the screen)
    std::string icon;                 // built-in icon name; default: the plugin's tile icon
    // Buttons: {action id, label}. Calls get "accept" / "decline" automatically.
    std::vector<std::pair<std::string, std::string>> actions;
    int timeout_s = 0;  // calls: stop ringing after this many seconds (0 = 45 s)

    // Notification distributors only (permission "notifications.distributor"):
    // post on behalf of another module, or of an app that is no module
    // (e.g. a Wayland client), shown under `app_name`.
    std::string source;
    std::string app_name;

    Json to_json() const;
};

// A Wayland client as reported by the compositor module (display.wayland provider).
struct WaylandClient {
    std::string module;   // the Facet module that started it
    std::string app_id;   // xdg_toplevel app id
    std::string title;
    int pid = 0;
    bool focused = false;
};

class Plugin {
public:
    Plugin(std::string id, std::string version);

    // Callbacks. All run on the thread that called run().
    std::function<void(const Json& hello)> on_hello;
    std::function<void(const std::string& id, const Json& value)> on_event;
    // "Done" pressed in a text field (the value is the final text).
    std::function<void(const std::string& id, const std::string& text)> on_submit;

    // ---- Keyboard plugins only (capability "input.keyboard").
    // The core asks for a keyboard: field mode ("text"/"number"), width in dp
    // and layout languages. Answer with keyboard_ui().
    std::function<void(const std::string& mode, float width, const std::vector<std::string>& langs)>
        on_keyboard_show;
    std::function<void(const std::string& hit)> on_keyboard_key;  // a key (hit id) was tapped
    std::function<void()> on_keyboard_hide;
    std::function<void(bool visible)> on_visible;
    // Called after the UI language changed (catalog already switched).
    std::function<void(const std::string& lang)> on_locale;
    // Called when the content width changed (e.g. another screen size).
    std::function<void(int content_width)> on_layout;
    std::function<void()> on_activity;
    std::function<void()> on_tick;
    std::function<void()> on_shutdown;

    // ---- Permissions. `permissions()` are those in effect (granted at start
    // plus transient grants now held). Optional permissions may be missing:
    // check before use and keep working without them.
    // Result of request_permission(), or a transient grant taken back by the user.
    std::function<void(const std::string& name, bool granted)> on_permission;

    // ---- Notifications.
    // A notification of this plugin was tapped ("open"), dismissed ("dismiss"),
    // a call answered ("accept" / "decline" / "timeout"), or a button pressed.
    std::function<void(const std::string& id, const std::string& action)> on_notification_action;
    // Distributors that called subscribe_notifications(): every notification
    // posted in the system, with "source" / "app_name".
    std::function<void(const Json& notification)> on_notification_posted;

    // ---- Wayland compositor modules (provide display.wayland, permission
    // wayland.compositor): a client module started or stopped, with the
    // wayland.* scopes the user granted it. Its own permissions are never
    // inherited from the compositor.
    std::function<void(const std::string& module, const std::vector<std::string>& scopes, bool running)>
        on_wayland_client;

    // ---- Surfaces. A finger on a surface, in its pixel coordinates
    // ("down", "move", "up"), and text typed on the core's keyboard while
    // text_input(true): action "insert" (with text), "backspace", "enter", "hide".
    std::function<void(const std::string& surface, const std::string& kind, float x, float y)> on_touch;
    std::function<void(const std::string& action, const std::string& text)> on_text;

    const std::string& data_dir() const { return data_dir_; }
    // Where Surface keeps its shared memory ("" on cores without surfaces).
    const std::string& surface_dir() const { return surface_dir_; }
    // The whole screen in pixels (a full-screen Surface should match it).
    int screen_width() const { return screen_w_; }
    int screen_height() const { return screen_h_; }
    // Width of the content column in dp: the width of canvas widgets.
    int content_width() const { return content_width_; }

    // Register translation tables here; the language follows the core.
    i18n::Catalog& catalog() { return catalog_; }
    std::string tr(std::string_view key) const { return catalog_.tr(key); }
    std::string tr(std::string_view key, const std::vector<std::string>& args) const { return catalog_.tr(key, args); }
    std::string render(const i18n::Text& t) const { return catalog_.render(t); }
    bool visible() const { return visible_; }
    const std::vector<std::string>& permissions() const { return permissions_; }
    bool has_permission(const std::string& name) const;

    // Senders skip messages identical to the previously sent value.
    void set_ui(const Screen& screen);
    void set_tile(const std::string& subtitle);
    void request_display(bool on);

    // Dynamic tile: a badge (e.g. unread count; 0 or "" hides it) and an icon,
    // either a built-in name or a drawing on a 24 x 24 grid.
    void set_badge(int count);
    void set_badge(const std::string& text);
    void set_tile_icon(const std::string& builtin);
    void set_tile_icon(const Canvas& icon);

    // Transient permissions ("while in use", e.g. the camera during a call):
    // ask for access now, answer in on_permission; release as soon as the
    // stream ends. Access is also withdrawn when the process exits.
    void request_permission(const std::string& name, const std::string& reason = {});
    void release_permission(const std::string& name);

    void notify(const Notification& n);
    void cancel_notification(const std::string& id, const std::string& source = {});
    // Distributors: receive copies of all notifications (on_notification_posted).
    void subscribe_notifications(bool on = true);

    // Background work, listed in Settings > Apps (permission "background";
    // without it the plugin is suspended while none of its screens is open).
    void begin_background(const std::string& reason);
    void end_background();
    // Keep the screen on while held (permission "wake_lock").
    void wake_lock(bool on);

    // Compositor modules: the Wayland clients that are running now.
    void report_wayland_clients(const std::vector<WaylandClient>& clients);

    // Ask the core to show its keyboard for this plugin's surface (typed
    // text arrives in on_text) or to hide it. mode: "text" or "number".
    void text_input(bool active, const std::string& mode = "text");
    // Keyboard plugins: current keyboard drawing and typed actions.
    void keyboard_ui(float height, const Canvas& canvas);
    void input(const std::string& action, const std::string& text = {});  // insert|backspace|enter|hide
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
    std::vector<std::string> permissions_;
    Json last_badge_, last_icon_;
    std::string surface_dir_;
    int screen_w_ = 0, screen_h_ = 0;
    std::map<std::string, Surface*> surfaces_;
    friend class Surface;
};

}  // namespace facet::sdk
