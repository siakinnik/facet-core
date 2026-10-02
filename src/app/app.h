#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/netstatus.h"
#include "facet/keyboard.h"
#include "gfx/canvas.h"
#include "platform/platform.h"
#include "plugins/plugin_host.h"
#include "ui/context.h"

namespace facet {

class App {
public:
    ~App();
    int run();

private:
    enum class View { Splash, Menu, Dashboard, Settings, Plugin, Apps, AppInfo, Notifications };

    bool init();
    bool load_fonts();
    void update_theme(bool force);
    void handle_events(const std::vector<platform::Event>& events, double t);
    void tick(double t);
    void update_display(double t);
    void run_frame(double t);
    // plugin_id: the plugin of Plugin and AppInfo views.
    void navigate(View v, const std::string& plugin_id = {});
    // Opens a plugin's screen; Back returns to `back` (Menu, Settings or AppInfo).
    void open_plugin(const std::string& id, View back);
    // remember=false: follow the environment until the user picks a language.
    void set_language(const std::string& lang, bool remember = true);
    // "" = system zone. Applies to the core and all plugins.
    void set_timezone(const std::string& zone);

    // Screens (screens.cpp).
    void draw_splash();
    void draw_menu();
    void draw_dashboard();
    void draw_settings(double t);
    void draw_timezone_settings();
    void draw_status_bar(float right, float cy);
    void draw_build_line(float y);
    void draw_plugin(double t);
    void draw_apps();
    void draw_app_info(double t);
    void leave_plugin();

    // Overlays and notifications (overlays.cpp).
    void prepare_overlays(double t);
    void draw_overlays(double t);
    bool modal_active(double t);
    gfx::Rect banner_rect() const;
    void draw_indicators();
    void draw_permission_prompt(const plugins::PermissionPrompt& q, double t);
    void draw_call(plugins::Notification& n, double t);
    void draw_banner(double t);
    void draw_notifications(double t);
    void open_module(const std::string& id);
    std::string module_name(const std::string& id) const;
    std::string notification_app(const plugins::Notification& n) const;
    ui::Icon notification_icon(const plugins::Notification& n) const;

    // On-screen keyboard (keyboard.cpp).
    void draw_keyboard();
    void close_keyboard();
    void apply_key(const std::string& action, const std::string& text);
    void take_plugin_input();
    std::string keyboard_plugin() const;  // selected, running keyboard plugin or "" (built-in)
    std::vector<std::string> keyboard_langs() const;
    void render_plugin_ui(plugins::Plugin& p);

    std::unique_ptr<platform::Platform> platform_;
    gfx::Canvas canvas_;
    ui::Fonts fonts_;
    ui::Theme theme_;
    ui::Context ui_;
    Config config_;
    plugins::PluginHost host_{config_};
    net::Monitor net_;

    View view_ = View::Splash;
    std::string plugin_id_;
    View plugin_back_ = View::Menu;  // where Back leads from a plugin screen
    bool dirty_ = true, drew_ = false;
    int drawn_minute_ = -1;
    bool night_ = false;

    // Splash / boot.
    int boot_stage_ = 0;
    float splash_progress_ = 0;
    double splash_start_ = 0, last_tick_ = 0;
    std::atomic<bool> system_ready_{false};
    std::atomic<bool> stop_{false};
    std::thread boot_thread_;

    // Input and display power.
    ui::Pointer pointer_;
    bool swallow_ = false;
    bool moved_ = false;  // pointer moved since the last frame
    bool debug_input_ = false;  // FACET_DEBUG_INPUT: log every down/up
    bool display_on_ = true;
    double last_input_ = 0, last_touch_ = 0, last_activity_sent_ = 0;

    std::string ip_cache_;
    std::optional<plugins::Notification> banner_;  // shown at the top until banner_until_
    double banner_until_ = 0;
    // Region picked in Settings while its city is not chosen yet.
    std::optional<std::string> tz_region_pending_;

    // Keyboard session: which field it serves and who draws it ("" = built-in).
    struct KeyboardState {
        bool visible = false;
        uint64_t field = 0;
        std::string plugin;
    } kb_;
    gfx::Rect kb_rect_;
    sdk::Keyboard builtin_kb_;  // also the fallback when the plugin is missing
    std::vector<std::pair<std::string, std::string>> kb_pending_;  // built-in keys, applied after the frame
};

}  // namespace facet
