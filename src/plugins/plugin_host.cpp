#include "plugins/plugin_host.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/log.h"

extern char** environ;

namespace facet::plugins {

namespace {

constexpr double kHelloTimeout = 5;
constexpr double kPingInterval = 5;
constexpr double kPongTimeout = 15;
constexpr double kStopTimeout = 2;
constexpr size_t kMaxLine = 1 << 20;
constexpr size_t kMaxOutbox = 1 << 20;
constexpr int kMaxBadLines = 10;
constexpr int kMaxCrashes = 5;
constexpr double kCrashWindow = 600;

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

// English, for logs. The UI renders Failure itself in the active language.
std::string describe(const Failure& f) {
    std::string s = i18n::format(f.reason.key, f.reason.args);
    if (f.wait_status >= 0) {
        if (WIFEXITED(f.wait_status)) s += " (exit code " + std::to_string(WEXITSTATUS(f.wait_status)) + ")";
        else if (WIFSIGNALED(f.wait_status)) s += std::string(" (signal ") + strsignal(WTERMSIG(f.wait_status)) + ")";
    }
    return s;
}

bool load_manifest(const std::string& dir, Manifest& m) {
    Json j;
    if (!load_json_file(dir + "/manifest.json", j) || !j.is_object()) return false;
    m.dir = dir;
    m.id = j["id"].str();
    m.name = LocalizedString::from(j["name"], m.id);
    m.version = j["version"].as_string("0");
    m.api = j["api"].as_int();
    m.exec = j["exec"].str();
    for (const auto& c : j["capabilities"].items()) m.capabilities.push_back(c.str());
    if (j["tile"].is_object()) {
        m.has_tile = true;
        m.tile_title = j["tile"]["title"].is_null() ? m.name : LocalizedString::from(j["tile"]["title"], m.id);
        m.tile_icon = j["tile"]["icon"].as_string("plugin");
    }
    if (!valid_id(m.id)) {
        log::warn("plugins: %s: invalid id", dir.c_str());
        return false;
    }
    if (m.api != 1) {
        log::warn("plugins: %s: unsupported api %d", m.id.c_str(), m.api);
        return false;
    }
    if (m.exec.empty() || m.exec[0] == '/' || m.exec.find("..") != std::string::npos) {
        log::warn("plugins: %s: bad exec", m.id.c_str());
        return false;
    }
    return true;
}

}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Disabled: return "disabled";
        case State::Starting: return "starting…";
        case State::Running: return "running";
        case State::Backoff: return "crashed, restarting";
        case State::Failed: return "failed";
        case State::Stopping: return "stopping…";
    }
    return "?";
}

std::string LocalizedString::get(const std::string& lang) const {
    auto it = values.find(lang);
    if (it == values.end()) it = values.find("en");
    if (it == values.end()) it = values.begin();
    return it == values.end() ? std::string() : it->second;
}

LocalizedString LocalizedString::from(const Json& j, const std::string& fallback) {
    LocalizedString out;
    if (j.is_string()) {
        out.values["en"] = j.str();
    } else {
        for (const auto& [lang, v] : j.fields())
            if (v.is_string()) out.values[lang] = v.str();
    }
    if (out.values.empty()) out.values["en"] = fallback;
    return out;
}

bool Manifest::can(const std::string& cap) const {
    return std::find(capabilities.begin(), capabilities.end(), cap) != capabilities.end();
}

PluginHost::~PluginHost() { shutdown_all(); }

// ------------------------------------------------------------------ discovery

void PluginHost::scan() {
    auto add = [this](const std::string& rel_dir) {
        // Absolute: the child chdir()s into it before exec.
        char abs[PATH_MAX];
        std::string dir = realpath(rel_dir.c_str(), abs) ? std::string(abs) : rel_dir;
        Manifest m;
        if (!load_manifest(dir, m)) return;
        if (find(m.id)) return;  // earlier search path wins
        auto p = std::make_unique<Plugin>();
        p->m = std::move(m);
        log::info("plugins: found %s %s in %s", p->m.id.c_str(), p->m.version.c_str(), dir.c_str());
        plugins_.push_back(std::move(p));
    };
    for (const auto& dir : paths::plugin_dirs()) {
        if (file_exists(dir + "/manifest.json")) {
            add(dir);
            continue;
        }
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> subdirs;
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') subdirs.push_back(dir + "/" + e->d_name);
        closedir(d);
        std::sort(subdirs.begin(), subdirs.end());
        for (const auto& s : subdirs)
            if (file_exists(s + "/manifest.json")) add(s);
    }
}

Plugin* PluginHost::find(const std::string& id) {
    for (auto& p : plugins_)
        if (p->m.id == id) return p.get();
    return nullptr;
}

bool PluginHost::is_enabled(const std::string& id) const {
    for (const auto& d : config_.get("plugins_disabled").items())
        if (d.str() == id) return false;
    return true;
}

void PluginHost::start_enabled(double now) {
    for (auto& p : plugins_)
        if (is_enabled(p->m.id)) spawn(*p, now);
}

bool PluginHost::all_settled() const {
    return std::none_of(plugins_.begin(), plugins_.end(), [](const auto& p) { return p->state == State::Starting; });
}

// ------------------------------------------------------------------ process management

void PluginHost::spawn(Plugin& p, double now) {
    std::string exe = p.m.dir + "/" + p.m.exec;
    if (::access(exe.c_str(), X_OK) != 0) {
        p.state = State::Failed;
        p.error = Failure{{"executable not found: {}", {p.m.exec}}};
        log::error("plugins: %s: %s", p.m.id.c_str(), exe.c_str());
        changed_ = true;
        return;
    }
    std::string data_dir = paths::data_root() + "/data/" + p.m.id;
    paths::mkdirs(data_dir);

    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) {
        crash(p, {"pipe failed: {}", {std::strerror(errno)}}, now);
        return;
    }

    // Everything the child needs is prepared before fork (no allocation after).
    std::vector<std::string> env_store;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "FACET_PLUGIN_", 13) == 0) continue;
        env_store.emplace_back(*e);
    }
    env_store.push_back("FACET_PLUGIN_ID=" + p.m.id);
    env_store.push_back("FACET_PLUGIN_DATA=" + data_dir);
    env_store.push_back("FACET_API=1");
    std::vector<char*> envp;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);
    char* argv[] = {exe.data(), nullptr};
    pid_t parent = getpid();

    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) ::close(fd);
        crash(p, {"fork failed: {}", {std::strerror(errno)}}, now);
        return;
    }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(1);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        signal(SIGPIPE, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        setpgid(0, 0);
        if (chdir(p.m.dir.c_str()) != 0) _exit(126);
        execve(argv[0], argv, envp.data());
        _exit(127);
    }

    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    p.pid = pid;
    p.fd_in = in[1];
    p.fd_out = out[0];
    p.fd_err = err[0];
    set_nonblock(p.fd_in);
    set_nonblock(p.fd_out);
    set_nonblock(p.fd_err);
    p.rbuf.clear();
    p.ebuf.clear();
    p.wbuf.clear();
    p.bad_lines = 0;
    p.state = State::Starting;
    p.started_at = now;
    p.deadline = now + kHelloTimeout;
    p.display.reset();
    changed_ = true;
    log::info("plugins: started %s (pid %d)", p.m.id.c_str(), pid);

    Json hello = Json::object();
    hello["t"] = "hello";
    hello["api"] = 1;
    hello["data_dir"] = data_dir;
    hello["theme"] = theme_;
    hello["locale"] = locale_;
    hello["timezone"] = timezone_;
    hello["content_width"] = content_width_;
    send(p, hello);
    if (p.visible) {
        Json v = Json::object();
        v["t"] = "visible";
        v["value"] = true;
        send(p, v);
    }
}

void PluginHost::close_fds(Plugin& p) {
    for (int* fd : {&p.fd_in, &p.fd_out, &p.fd_err}) {
        if (*fd >= 0) ::close(*fd);
        *fd = -1;
    }
}

void PluginHost::kill_now(Plugin& p) {
    if (p.pid <= 0) return;
    ::kill(p.pid, SIGKILL);
    int st = 0;
    while (waitpid(p.pid, &st, 0) < 0 && errno == EINTR) {
    }
    p.pid = -1;
    close_fds(p);
}

void PluginHost::crash(Plugin& p, i18n::Text reason, double now) {
    Failure f{std::move(reason)};
    if (p.pid > 0) {
        int st = 0;
        pid_t r = waitpid(p.pid, &st, WNOHANG);
        for (int i = 0; i < 10 && r == 0; ++i) {  // closed pipe usually means it is exiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            r = waitpid(p.pid, &st, WNOHANG);
        }
        if (r == p.pid) {
            f.wait_status = st;
            p.pid = -1;
            close_fds(p);
        } else {
            kill_now(p);
        }
    }
    close_fds(p);
    p.display.reset();  // fail-safe: whatever it held is released
    p.ui = Json();
    changed_ = true;

    p.crashes.push_back(now);
    p.crashes.erase(std::remove_if(p.crashes.begin(), p.crashes.end(),
                                   [now](double t) { return now - t > kCrashWindow; }),
                    p.crashes.end());
    if (int(p.crashes.size()) >= kMaxCrashes) {
        f.gave_up = true;
        p.error = f;
        p.state = State::Failed;
        log::error("plugins: %s failed permanently: %s", p.m.id.c_str(), describe(f).c_str());
        return;
    }
    p.error = f;
    double delay = std::min(60.0, double(1 << (p.crashes.size() - 1)));
    p.state = State::Backoff;
    p.restart_at = now + delay;
    log::warn("plugins: %s crashed: %s; restart in %.0fs", p.m.id.c_str(), describe(f).c_str(), delay);
}

void PluginHost::on_exit(Plugin& p, int status, double now) {
    p.pid = -1;
    if (p.state == State::Stopping) {
        close_fds(p);
        p.state = State::Disabled;
        p.display.reset();
        p.ui = Json();
        changed_ = true;
        log::info("plugins: %s stopped", p.m.id.c_str());
        return;
    }
    crash(p, "process exited", now);
    p.error.wait_status = status;  // already reaped, so crash() could not see it
}

void PluginHost::shutdown_all() {
    Json bye = Json::object();
    bye["t"] = "shutdown";
    bool any = false;
    for (auto& p : plugins_) {
        if (p->pid <= 0) continue;
        send(*p, bye);
        flush(*p);
        if (p->fd_in >= 0) {
            ::close(p->fd_in);  // EOF also tells it to exit
            p->fd_in = -1;
        }
        any = true;
    }
    for (int i = 0; any && i < 50; ++i) {
        any = false;
        for (auto& p : plugins_) {
            if (p->pid <= 0) continue;
            int st;
            if (waitpid(p->pid, &st, WNOHANG) == p->pid) {
                p->pid = -1;
                close_fds(*p);
            } else {
                any = true;
            }
        }
        if (any) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& p : plugins_) kill_now(*p);
}

// ------------------------------------------------------------------ IPC

void PluginHost::send(Plugin& p, const Json& msg) {
    if (p.fd_in < 0) return;
    p.wbuf += msg.dump();
    p.wbuf += '\n';
}

void PluginHost::flush(Plugin& p) {
    while (!p.wbuf.empty() && p.fd_in >= 0) {
        ssize_t n = ::write(p.fd_in, p.wbuf.data(), p.wbuf.size());
        if (n > 0) {
            p.wbuf.erase(0, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;  // EAGAIN: retry later; EPIPE: exit is detected via EOF/waitpid
    }
}

void PluginHost::drain_stderr(Plugin& p) {
    char buf[4096];
    while (p.fd_err >= 0) {
        ssize_t n = ::read(p.fd_err, buf, sizeof buf);
        if (n <= 0) break;
        p.ebuf.append(buf, size_t(n));
        size_t nl;
        while ((nl = p.ebuf.find('\n')) != std::string::npos) {
            log::info("[%s] %s", p.m.id.c_str(), p.ebuf.substr(0, nl).c_str());
            p.ebuf.erase(0, nl + 1);
        }
        if (p.ebuf.size() > 4096) p.ebuf.clear();
    }
}

void PluginHost::read_pipes(Plugin& p, double now) {
    drain_stderr(p);
    char buf[8192];
    bool eof = false;
    while (p.fd_out >= 0) {
        ssize_t n = ::read(p.fd_out, buf, sizeof buf);
        if (n == 0) {
            eof = true;
            break;
        }
        if (n < 0) break;
        p.rbuf.append(buf, size_t(n));
    }
    size_t nl;
    while (p.pid > 0 && (nl = p.rbuf.find('\n')) != std::string::npos) {
        std::string line = p.rbuf.substr(0, nl);
        p.rbuf.erase(0, nl + 1);
        Json msg;
        if (Json::parse(line, msg) && msg.is_object()) {
            handle(p, msg, now);
        } else if (++p.bad_lines > kMaxBadLines) {
            crash(p, "protocol violation (invalid messages)", now);
            return;
        }
    }
    if (p.pid > 0 && p.rbuf.size() > kMaxLine) {
        crash(p, "protocol violation (message too long)", now);
        return;
    }
    if (eof && p.pid > 0) {
        if (p.state == State::Stopping) {
            kill_now(p);
            on_exit(p, 0, now);
        } else {
            crash(p, "process closed its output", now);
        }
    }
}

void PluginHost::handle(Plugin& p, const Json& msg, double now) {
    const std::string& t = msg["t"].str();
    if (p.state == State::Starting) {
        if (t != "hello") return;
        if (msg["api"].as_int() != 1 || msg["id"].str() != p.m.id) {
            crash(p, "invalid hello", now);
            return;
        }
        p.state = State::Running;
        p.error = {};
        p.last_pong = now;
        p.next_ping = now + kPingInterval;
        changed_ = true;
        log::info("plugins: %s ready (%s)", p.m.id.c_str(), msg["version"].str().c_str());
        return;
    }
    if (t == "pong") {
        p.last_pong = now;
    } else if (t == "ui") {
        if (msg["root"].is_object()) {
            p.ui = msg["root"];
            changed_ = true;
        }
    } else if (t == "tile") {
        p.tile_subtitle = msg["subtitle"].str();
        changed_ = true;
    } else if (t == "display") {
        if (!p.m.can("display.power")) {
            log::warn("plugins: %s: display request without display.power capability", p.m.id.c_str());
            return;
        }
        p.display = msg["on"].as_bool(true);
        changed_ = true;
    }
}

// ------------------------------------------------------------------ loop integration

void PluginHost::add_poll_fds(std::vector<pollfd>& fds) const {
    for (const auto& p : plugins_) {
        if (p->fd_out >= 0) fds.push_back({p->fd_out, POLLIN, 0});
        if (p->fd_err >= 0) fds.push_back({p->fd_err, POLLIN, 0});
        if (p->fd_in >= 0 && !p->wbuf.empty()) fds.push_back({p->fd_in, POLLOUT, 0});
    }
}

void PluginHost::process(double now) {
    for (auto& ptr : plugins_) {
        Plugin& p = *ptr;
        if (p.pid > 0) read_pipes(p, now);
        if (p.pid > 0) {
            int st = 0;
            if (waitpid(p.pid, &st, WNOHANG) == p.pid) {
                p.pid = -1;  // reaped: never signal this pid again
                drain_stderr(p);
                on_exit(p, st, now);
            }
        }
        switch (p.state) {
            case State::Starting:
                if (now > p.deadline) crash(p, "no hello within 5 s", now);
                break;
            case State::Running:
                if (now - p.last_pong > kPongTimeout) {
                    crash(p, "hung (no reply to ping)", now);
                } else if (now >= p.next_ping) {
                    Json ping = Json::object();
                    ping["t"] = "ping";
                    ping["seq"] = ++p.ping_seq;
                    send(p, ping);
                    p.next_ping = now + kPingInterval;
                }
                break;
            case State::Stopping:
                if (now > p.deadline) {
                    kill_now(p);
                    on_exit(p, 0, now);
                }
                break;
            case State::Backoff:
                if (now >= p.restart_at) spawn(p, now);
                break;
            default: break;
        }
        if (p.pid > 0) {
            flush(p);
            if (p.wbuf.size() > kMaxOutbox) crash(p, "not reading messages", now);
        }
    }
}

double PluginHost::next_deadline(double now) const {
    double t = now + 3600;
    for (const auto& p : plugins_) {
        switch (p->state) {
            case State::Starting:
            case State::Stopping: t = std::min(t, p->deadline); break;
            case State::Running: t = std::min({t, p->next_ping, p->last_pong + kPongTimeout}); break;
            case State::Backoff: t = std::min(t, p->restart_at); break;
            default: break;
        }
    }
    return t;
}

bool PluginHost::take_changed() {
    bool c = changed_;
    changed_ = false;
    return c;
}

// ------------------------------------------------------------------ control

void PluginHost::set_enabled(const std::string& id, bool enabled, double now) {
    Plugin* p = find(id);
    if (!p) return;
    Json list = Json::array();
    for (const auto& d : config_.get("plugins_disabled").items())
        if (d.str() != id) list.push_back(d);
    if (!enabled) list.push_back(id);
    config_.set("plugins_disabled", list);

    if (enabled) {
        if (p->state == State::Disabled || p->state == State::Failed) {
            p->crashes.clear();
            spawn(*p, now);
        }
    } else if (p->pid > 0) {
        Json bye = Json::object();
        bye["t"] = "shutdown";
        send(*p, bye);
        p->state = State::Stopping;
        p->deadline = now + kStopTimeout;
    } else {
        p->state = State::Disabled;
    }
    p->display.reset();
    changed_ = true;
}

void PluginHost::restart(const std::string& id, double now) {
    Plugin* p = find(id);
    if (!p || !is_enabled(id)) return;
    kill_now(*p);
    p->crashes.clear();
    p->error = {};
    spawn(*p, now);
}

void PluginHost::set_visible(const std::string& id, bool visible) {
    Plugin* p = find(id);
    if (!p || p->visible == visible) return;
    p->visible = visible;
    if (p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "visible";
    msg["value"] = visible;
    send(*p, msg);
}

void PluginHost::send_event(const std::string& id, const std::string& widget, const Json& value) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "event";
    msg["id"] = widget;
    msg["value"] = value;
    send(*p, msg);
}

void PluginHost::broadcast_activity() {
    Json msg = Json::object();
    msg["t"] = "activity";
    for (auto& p : plugins_)
        if (p->state == State::Running && p->m.can("display.power")) send(*p, msg);
}

void PluginHost::set_theme(const std::string& theme) { theme_ = theme; }

void PluginHost::set_content_width(int dp) {
    if (dp == content_width_) return;
    content_width_ = dp;
    Json msg = Json::object();
    msg["t"] = "layout";
    msg["content_width"] = dp;
    for (auto& p : plugins_)
        if (p->state == State::Running) send(*p, msg);
}

void PluginHost::set_timezone(const std::string& zone) {
    if (zone == timezone_) return;
    timezone_ = zone;
    Json msg = Json::object();
    msg["t"] = "timezone";
    msg["value"] = zone;
    for (auto& p : plugins_)
        if (p->state == State::Running) send(*p, msg);
}

void PluginHost::set_locale(const std::string& lang) {
    if (lang == locale_) return;
    locale_ = lang;
    Json msg = Json::object();
    msg["t"] = "locale";
    msg["value"] = lang;
    for (auto& p : plugins_)
        if (p->state == State::Running) send(*p, msg);
}

std::optional<bool> PluginHost::display_policy() const {
    for (const auto& p : plugins_)
        if (p->state == State::Running && p->m.can("display.power") && p->display) return p->display;
    return std::nullopt;
}

}  // namespace facet::plugins
