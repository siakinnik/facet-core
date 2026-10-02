// Out-of-process plugin supervisor. See docs/ARCHITECTURE.md §3.
#pragma once

#include <poll.h>
#include <sys/types.h>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "facet/i18n.h"
#include "facet/json.h"
#include "plugins/notifications.h"
#include "plugins/permissions.h"

namespace facet::plugins {

enum class State { Disabled, Blocked, Starting, Running, Backoff, Failed, Stopping };
// English key of the state name; translate with tr().
const char* state_name(State s);

// A manifest string: either plain, or {"en": "...", "ru": "..."}.
struct LocalizedString {
    std::map<std::string, std::string> values;
    std::string get(const std::string& lang) const;  // lang, then "en", then any
    static LocalizedString from(const Json& j, const std::string& fallback);
};

// A versioned capability a plugin provides or requires ("input.keyboard@1").
struct Capability {
    std::string name;
    int version = 1;
    std::string install;  // requires only: where to get a provider ("owner/repo"), may be empty
    std::string str() const { return name + "@" + std::to_string(version); }
};

// One entry of the manifest's "permissions".
struct PermissionRequest {
    std::string name;
    bool optional = false;   // the plugin works without it (otherwise it is not started)
    bool transient = false;  // asked for at run time, held only while in use
    LocalizedString reason;  // why the plugin needs it (shown to the user), may be empty
};

struct Manifest {
    std::string id, version, exec, dir;
    std::string sdk;  // SDK the plugin was built with, "" if unknown
    LocalizedString name, tile_title, settings_title;
    int api = 0;
    std::vector<PermissionRequest> permissions;  // requested; the user grants them
    const PermissionRequest* permission(const std::string& name) const;
    std::vector<Capability> provides, needs;  // "provides" / "requires" in the manifest
    std::string tile_icon;
    bool has_tile = false;      // a tile on the home screen
    bool has_settings = false;  // a page under Settings > Modules
    bool compatible() const;
    bool provides_cap(const std::string& name) const;
};

// Why a plugin is not started even though it is enabled.
enum class Block { None, Incompatible, NeedsReview, MissingPermission, MissingDependency };

// Why a plugin stopped, kept untranslated until shown.
struct Failure {
    i18n::Text reason;
    int wait_status = -1;  // from waitpid(), -1 if unknown
    bool gave_up = false;  // too many crashes: no more automatic restarts
    bool empty() const { return reason.empty(); }
};

// A plugin's Surface: its shared memory, mapped read-only.
struct SurfaceBuffer {
    int w = 0, h = 0, stride = 0, buffers = 0;
    int current = -1;  // buffer shown now, -1 before the first frame
    const uint8_t* map = nullptr;
    size_t size = 0;
    const uint32_t* pixels() const {
        return current < 0 ? nullptr
                           : reinterpret_cast<const uint32_t*>(map + size_t(current) * size_t(stride) * size_t(h));
    }
};

struct Plugin {
    Manifest m;
    State state = State::Disabled;
    Failure error;  // last failure, shown to the user
    Block block = Block::None;
    i18n::Text block_reason;          // untranslated, shown with the block
    std::vector<std::string> granted;  // persistent permissions of the running process
    std::vector<std::string> transient;  // transient permissions it holds right now
    bool sandboxed = false;            // the running process is in a container
    std::string sdk_reported;          // SDK version from the plugin's hello

    // Process + pipes.
    pid_t pid = -1;
    int fd_in = -1, fd_out = -1, fd_err = -1;
    std::string rbuf, ebuf, wbuf;
    int bad_lines = 0;

    double started_at = 0, last_pong = 0, next_ping = 0, deadline = 0, restart_at = 0;
    int ping_seq = 0;
    std::vector<double> crashes;

    // State published by the plugin.
    Json ui;
    std::string tile_subtitle;
    std::optional<bool> display;
    bool visible = false;

    // Keyboard plugins: the drawing they last sent for the open keyboard.
    Json keyboard_ops;
    float keyboard_height = 0;

    // Dynamic tile.
    std::string badge;
    std::string tile_icon_name;  // overrides the manifest icon when set
    Json tile_icon_ops;          // custom drawing on a 24 x 24 grid

    // Background.
    bool frozen = false;          // suspended (no "background" permission, not on screen)
    double hidden_since = 0;
    std::string background_task;  // reason of the current background work, "" if none
    bool wake_lock = false;
    bool subscribed = false;      // distributor receiving all notifications
    // Transient permission the user withdrew; the plugin must release it by then.
    std::map<std::string, double> revoke_deadline;

    // Surfaces and keyboard input for them.
    std::map<std::string, SurfaceBuffer> surfaces;
    bool text_input = false;
    std::string text_mode;

    bool holds(const std::string& perm) const;  // persistent or transient
    const SurfaceBuffer* surface(const std::string& id) const;
};

// A runtime permission request waiting for the user's answer.
struct PermissionPrompt {
    std::string plugin, permission, reason;
};

// A Wayland client as reported by the compositor module.
struct WaylandClient {
    std::string module, app_id, title;
    int pid = 0;
    bool focused = false;
};

// A plugin that runs while none of its screens is open.
struct BackgroundEntry {
    std::string plugin;
    std::string task;  // what it says it is doing, may be empty
};

class PluginHost {
public:
    explicit PluginHost(Config& config) : config_(config) {}
    ~PluginHost();

    void scan();
    void start_enabled(double now);
    void shutdown_all();

    void add_poll_fds(std::vector<pollfd>& fds) const;
    // Reads pipes, reaps children, runs watchdog/restart timers.
    void process(double now);
    // Earliest time process() must run again.
    double next_deadline(double now) const;
    // True if anything user-visible changed since the last call.
    bool take_changed();

    const std::vector<std::unique_ptr<Plugin>>& plugins() const { return plugins_; }
    Plugin* find(const std::string& id);
    bool all_settled() const;  // nothing still in Starting

    void set_enabled(const std::string& id, bool enabled, double now);
    bool is_enabled(const std::string& id) const;

    // ---- Permissions (Settings > Apps). Stored in the config as
    // permissions.<id>.<permission> = "allow" | "ask" | "deny". A plugin starts
    // once every dangerous or special permission it requests has a choice.
    Grant grant(const std::string& id, const std::string& perm) const;  // stored choice
    // The choice shown for an unreviewed permission (required: allow,
    // optional: deny, transient: ask; normal permissions: allow).
    static Grant default_grant(const PermissionRequest& r);
    Grant shown_grant(const std::string& id, const PermissionRequest& r) const;
    void set_grant(const std::string& id, const std::string& perm, Grant g, double now);
    // Stores the shown choice for every unreviewed permission and starts the plugin.
    void confirm_permissions(const std::string& id, double now);

    // Runtime requests of transient permissions waiting for the user.
    const PermissionPrompt* pending_prompt() const;
    // allow: this time; always: also from now on without asking.
    void answer_prompt(bool allow, bool always, double now);
    // The user stops a transient grant (e.g. the camera indicator).
    void revoke_transient(const std::string& id, const std::string& perm, double now);

    // ---- Notifications.
    NotificationCenter& notifications() { return notifications_; }
    // The user acted on a notification: "open", "dismiss", "accept", "decline" or a button id.
    void notification_action(uint64_t serial, const std::string& action, double now);
    // Until when the screen must stay on for a new notification or a call.
    double wake_until() const { return wake_until_; }

    // ---- Registries shown in Settings > Apps.
    std::vector<BackgroundEntry> background_registry() const;
    const std::vector<WaylandClient>& wayland_clients() const { return wayland_clients_; }
    // Enabled plugins that are blocked or failed and need the user.
    int attention_count() const;
    // Plugins run in containers (Facet runs as root, FACET_SANDBOX != 0).
    bool sandbox_active() const;
    void restart(const std::string& id, double now);
    void set_visible(const std::string& id, bool visible);
    void send_event(const std::string& id, const std::string& widget, const Json& value,
                    const std::string& action = {});
    void broadcast_activity();
    void set_theme(const std::string& theme);
    void set_locale(const std::string& lang);
    void set_timezone(const std::string& zone);  // "" = system zone
    void set_content_width(int dp);              // width of canvas widgets
    void set_screen_size(int w, int h);          // in pixels, for full-screen surfaces
    // Input for a plugin's surface: a finger in surface pixels, or typed text.
    void send_touch(const std::string& id, const std::string& surface, const char* kind, float x, float y);
    void send_text(const std::string& id, const std::string& action, const std::string& text);

    // Screen policy from a running plugin with display.power, if any; wake
    // locks and ringing calls force the screen on.
    std::optional<bool> display_policy(double now);

    // ---- Keyboard plugins (provide input.keyboard).
    bool keyboard_ready(const std::string& id) const;  // running and has drawn itself
    void keyboard_show(const std::string& id, const std::string& mode, float width,
                       const std::vector<std::string>& langs);
    void keyboard_key(const std::string& id, const std::string& hit);
    void keyboard_hide(const std::string& id);
    struct InputAction {
        std::string plugin, action, text;  // action: insert|backspace|enter|hide
    };
    std::vector<InputAction> take_input();

private:
    void spawn(Plugin& p, double now);
    // Recomputes every plugin's Block and starts/stops plugins accordingly.
    void refresh_blocks(double now);
    // Persistent permissions the process gets at start.
    std::vector<std::string> start_permissions(const Plugin& p) const;
    uid_t plugin_uid(const std::string& id);
    void handle_permission_request(Plugin& p, const std::string& perm, const std::string& reason, double now);
    void grant_transient(Plugin& p, const std::string& perm, bool granted, double now);
    void drop_transient(Plugin& p, const std::string& perm);
    void handle_notify(Plugin& p, const Json& msg, double now);
    void freeze(Plugin& p, bool on, double now);
    void reset_runtime(Plugin& p);
    void handle_surface(Plugin& p, const Json& msg);
    void unmap_surfaces(Plugin& p);
    void tell_compositor(Plugin& client, bool running);
    Plugin* compositor();
    void send(Plugin& p, const Json& msg);
    void flush(Plugin& p);
    void read_pipes(Plugin& p, double now);
    void drain_stderr(Plugin& p);
    void handle(Plugin& p, const Json& msg, double now);
    void kill_now(Plugin& p);
    void on_exit(Plugin& p, int status, double now);
    void crash(Plugin& p, i18n::Text reason, double now);
    void close_fds(Plugin& p);

    Config& config_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::string theme_ = "dark";
    std::string locale_ = "en";
    std::string timezone_;
    int content_width_ = 440;
    int screen_w_ = 0, screen_h_ = 0;
    std::vector<InputAction> input_;
    bool changed_ = true;
    std::deque<PermissionPrompt> prompts_;
    NotificationCenter notifications_;
    double wake_until_ = 0;
    std::vector<WaylandClient> wayland_clients_;
};

}  // namespace facet::plugins
