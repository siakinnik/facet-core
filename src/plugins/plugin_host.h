// Out-of-process plugin supervisor. See docs/ARCHITECTURE.md §3.
#pragma once

#include <poll.h>
#include <sys/types.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "facet/i18n.h"
#include "facet/json.h"

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
    std::string str() const { return name + "@" + std::to_string(version); }
};

struct Manifest {
    std::string id, version, exec, dir;
    std::string sdk;  // SDK the plugin was built with, "" if unknown
    LocalizedString name, tile_title, settings_title;
    int api = 0;
    std::vector<std::string> permissions;  // requested; the user grants them
    std::vector<Capability> provides, requires;
    std::string tile_icon;
    bool has_tile = false;      // a tile on the home screen
    bool has_settings = false;  // a page under Settings > Modules
    bool compatible() const;
    bool provides_cap(const std::string& name) const;
};

// Why a plugin is not started even though it is enabled.
enum class Block { None, Incompatible, NeedsReview, MissingDependency };

// Why a plugin stopped, kept untranslated until shown.
struct Failure {
    i18n::Text reason;
    int wait_status = -1;  // from waitpid(), -1 if unknown
    bool gave_up = false;  // too many crashes: no more automatic restarts
    bool empty() const { return reason.empty(); }
};

struct Plugin {
    Manifest m;
    State state = State::Disabled;
    Failure error;  // last failure, shown to the user
    Block block = Block::None;
    i18n::Text block_reason;          // untranslated, shown with the block
    std::vector<std::string> granted;  // permissions in effect for the running process
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
    // permissions.<id> = {granted: [...], asked: [...]}; a plugin starts once
    // the user has seen every permission it requests.
    bool permission_granted(const std::string& id, const std::string& perm) const;
    void set_permission(const std::string& id, const std::string& perm, bool granted, double now);
    // Marks all requested permissions as seen (with their current grants) and starts the plugin.
    void confirm_permissions(const std::string& id, double now);
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

    // Screen policy from a running plugin with display.power, if any.
    std::optional<bool> display_policy() const;

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
    std::vector<std::string> asked(const std::string& id) const;
    std::vector<std::string> granted_list(const std::string& id) const;
    uid_t plugin_uid(const std::string& id);
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
    std::vector<InputAction> input_;
    bool changed_ = true;
};

}  // namespace facet::plugins
