#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/netstatus.h"
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
    enum class View { Splash, Menu, Dashboard, Settings, Plugin };

    bool init();
    bool load_fonts();
    void update_theme(bool force);
    void handle_events(const std::vector<platform::Event>& events, double t);
    void tick(double t);
    void update_display(double t);
    void run_frame(double t);
    void navigate(View v, const std::string& plugin_id = {});
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
    // Region picked in Settings while its city is not chosen yet.
    std::optional<std::string> tz_region_pending_;
};

}  // namespace facet
