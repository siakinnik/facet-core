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

enum class State { Disabled, Starting, Running, Backoff, Failed, Stopping };
// English key of the state name; translate with tr().
const char* state_name(State s);

// A manifest string: either plain, or {"en": "...", "ru": "..."}.
struct LocalizedString {
    std::map<std::string, std::string> values;
    std::string get(const std::string& lang) const;  // lang, then "en", then any
    static LocalizedString from(const Json& j, const std::string& fallback);
};

struct Manifest {
    std::string id, version, exec, dir;
    LocalizedString name, tile_title;
    int api = 0;
    std::vector<std::string> capabilities;
    std::string tile_icon;
    bool has_tile = false;
    bool can(const std::string& cap) const;
};

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
    void restart(const std::string& id, double now);
    void set_visible(const std::string& id, bool visible);
    void send_event(const std::string& id, const std::string& widget, const Json& value);
    void broadcast_activity();
    void set_theme(const std::string& theme);
    void set_locale(const std::string& lang);
    void set_timezone(const std::string& zone);  // "" = system zone

    // Screen policy from a running plugin with display.power, if any.
    std::optional<bool> display_policy() const;

private:
    void spawn(Plugin& p, double now);
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
    bool changed_ = true;
};

}  // namespace facet::plugins
