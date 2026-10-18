#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/netstatus.h"
#include "facet/keyboard.h"
#include "gpu/gl_package.h"
#include "gfx/canvas.h"
#include "gfx/recorder.h"
#include "platform/console_log.h"
#include "platform/evdev_mouse.h"
#include "platform/platform.h"
#include "plugins/plugin_host.h"
#include "ui/context.h"
#include "ui/skin.h"

namespace facet {

class App {
public:
    ~App();
    int run();

private:
    enum class View { Splash, Menu, Dashboard, Settings, Graphics, Plugin, Apps, AppInfo, Notifications };

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
    // Settings > Graphics: CPU or GPU, the graphics card, the OpenGL package.
    void draw_graphics();
    // First start / after an update: how to draw, then which graphics card
    // and how much video memory; then the download and the restart.
    enum class GfxDialog { None, Offer, Card, Memory, Restart };
    GfxDialog gfx_dialog_ = GfxDialog::None;
    void finish_gfx_setup();
    void update_gfx_dialog();
    void draw_gfx_dialog();
    // A centred card with a title, text and buttons; returns the clicked id.
    std::string draw_dialog(const std::string& key, const std::string& title, const std::string& text,
                            const std::vector<std::pair<std::string, std::string>>& buttons);
    void request_restart();
    // Power: restart only Facet, or reboot / shut down the whole device (a
    // dialog asks first). Offered where there is a real display.
    enum class PowerDialog { None, Facet, Reboot, Off, NotRoot, Going };
    PowerDialog power_dialog_ = PowerDialog::None;
    void draw_power_dialog();
    void power_now(bool reboot);
    int power_exit_ = 0;  // LINUX_REBOOT_CMD_* to call when the loop has ended (no init system)
    // A mouse beside the touchscreen (Settings > Input).
    platform::EvdevMouse mouse_;
    bool real_display_ = false;  // fbdev or gpu: a device, not a window
    bool cursor_ = false;        // the pointer is drawn (a mouse moved last)
    void draw_cursor();
    std::vector<gpu::GpuInfo> gpus_;
    std::unique_ptr<gpu::GlPackage> gl_;
    bool restart_needed_ = false;
    // Video memory Facet may use for its picture (bytes, 0 = no limit):
    // surfaces past it are copied into the canvas on the CPU instead.
    uint64_t vram_budget_ = 0, frame_vram_ = 0;
    uint64_t vram_base() const;  // canvas + scanout buffers
    std::vector<std::pair<std::string, std::string>> cpu_surfaces_;  // drawn into the canvas this frame
    bool cpu_surface_frames_ = false;  // the last frame had such surfaces
    double gfx_refresh_ = 0;
    int gfx_phase_ = -1;
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
    // Surfaces (screens.cpp): a widget or the whole screen; forwards touches
    // to the surface's owner (the plugin itself or a provider that lent it).
    void draw_surface(plugins::Plugin& p, const std::string& id, const gfx::Rect& r);
    void draw_fullscreen_surface(const plugins::SurfaceRef& s, const std::string& id);
    void track_surface_touch(const std::string& owner, const std::string& id, const gfx::Rect& r, int sw, int sh,
                             bool fullscreen);

    std::unique_ptr<platform::Platform> platform_;
    platform::ConsoleLog console_log_;  // quiet while a real display runs
    gfx::Canvas canvas_;
    ui::Fonts fonts_;
    ui::Theme theme_;
    ui::Context ui_;
    Config config_;
    plugins::PluginHost host_{config_};
    net::Monitor net_;

    // A theme pack in use (Settings > Theme), nothing for the built-in themes.
    std::shared_ptr<ui::Skin> skin_;
    std::string skin_key_;        // "module/theme" it was loaded for
    bool skin_changed_ = false;   // the theme has to be applied again
    bool anim_on_ = false;        // moving parts of drawings move
    uint32_t skin_serial_ = 0;    // counts the themes loaded (part of what a kept layer depends on)
    std::unordered_map<uint32_t, bool> layer_animated_;  // theme layer id -> it moves
    double frame_t_ = 0;          // when the last frame was drawn
    bool fullscreen_shown_ = false;  // a surface covers the screen this frame
    void draw_skin_screen(bool over);
    static const char* view_name(View v);

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
    // FACET_DEBUG_FRAMES: frames per second and where their time goes, every 5 s.
    bool debug_frames_ = false;
    double bench_next_ = 0, bench_last_ = 0, bench_max_ = 0;
    int bench_late_ = 0;
    double bench_hz_ = 60;
    // The frame counter in the corner (Settings > Graphics).
    bool show_fps_ = false;
    std::deque<double> fps_times_;
    double fps_draw_ms_ = 0;
    void draw_fps_counter(double t);
    bool bench_ = false;  // FACET_BENCH: draw continuously, whatever changed
    int stat_frames_ = 0;
    double stat_draw_ = 0, stat_present_ = 0, stat_since_ = 0, stat_layers_ = 0;
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
        std::string surface_owner;  // typing goes to this plugin's surface, not to a field
    } kb_;
    // A finger that went down on a surface: it belongs to that surface until it lifts.
    struct SurfaceTouch {
        bool active = false, gesture = false;
        std::string plugin, surface;
        float start_y = 0, last_x = -1, last_y = -1;
    } touch_;
    std::vector<std::string> surface_owners_;  // owners of the surfaces on screen now
    // GPU composition: the surfaces of the last frame, drawn under the canvas.
    struct LayerRef {
        std::string owner, id;
        gfx::Rect dst, clip;
    };
    std::vector<LayerRef> layer_refs_;
    bool layers_ = false;  // the platform composes surfaces itself
    bool ops_ = false;     // ... and draws the interface from recorded commands
    bool ops_unsent_ = false;  // a recorded frame waits for the helper to read the last one
    gfx::Recorder rec_;
    void present_frame(bool canvas_changed);
    void put_surface(const plugins::SurfaceRef& s, const std::string& id, const gfx::Rect& dst, const gfx::Rect& clip);
    gfx::Rect kb_rect_;
    sdk::Keyboard builtin_kb_;  // also the fallback when the plugin is missing
    std::vector<std::pair<std::string, std::string>> kb_pending_;  // built-in keys, applied after the frame
};

}  // namespace facet
