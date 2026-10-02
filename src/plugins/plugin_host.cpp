#include "plugins/plugin_host.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "core/log.h"
#include "facet/plugin.h"
#include "plugins/sandbox.h"

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
constexpr double kFreezeAfter = 10;     // seconds hidden before a plugin without "background" is paused
constexpr double kRevokeGrace = 5;      // seconds to release a withdrawn transient permission
constexpr double kNotificationWake = 10;  // screen on after an important notification

double mono_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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

// Names end up in paths (capability endpoints): the same rules as plugin ids.
Capability parse_capability(const std::string& s) {
    Capability c;
    size_t at = s.find('@');
    c.name = s.substr(0, at);
    if (at != std::string::npos) c.version = std::max(1, std::atoi(s.c_str() + at + 1));
    if (!valid_id(c.name) || c.name.find("..") != std::string::npos || c.name[0] == '.') c.name.clear();
    return c;
}

// Plugins with another API version are kept (and shown as incompatible), so
// only what identifies them must be valid.
bool load_manifest(const std::string& dir, Manifest& m) {
    Json j;
    if (!load_json_file(dir + "/manifest.json", j) || !j.is_object()) return false;
    m.dir = dir;
    m.id = j["id"].str();
    m.name = LocalizedString::from(j["name"], m.id);
    m.version = j["version"].as_string("0");
    m.sdk = j["sdk"].str();
    m.api = j["api"].as_int();
    m.exec = j["exec"].str();
    for (const auto& c : j["permissions"].items()) {
        PermissionRequest r;
        if (c.is_string()) {
            r.name = c.str();
        } else {
            r.name = c["name"].str();
            r.optional = c["optional"].as_bool();
            r.transient = c["transient"].as_bool();
            if (!c["reason"].is_null()) r.reason = LocalizedString::from(c["reason"], "");
        }
        const PermissionInfo* info = permission_info(r.name);
        if (r.transient && !(info && info->can_be_transient)) {
            log::warn("plugins: %s: %s cannot be transient", m.id.c_str(), r.name.c_str());
            r.transient = false;
        }
        if (!r.name.empty() && !m.permission(r.name)) m.permissions.push_back(std::move(r));
    }
    for (const auto& c : j["provides"].items()) {
        // "name@version" or {"name": "name@version", "endpoint": true}
        Capability cap = parse_capability(c.is_string() ? c.str() : c["name"].str());
        cap.endpoint = c["endpoint"].as_bool();
        if (!cap.name.empty()) m.provides.push_back(std::move(cap));
    }
    for (const auto& c : j["requires"].items()) {
        // "name@version" or {"name": "name@version", "install": "owner/repo"}
        Capability cap = parse_capability(c.is_string() ? c.str() : c["name"].str());
        cap.install = c["install"].str();
        if (!cap.name.empty()) m.needs.push_back(std::move(cap));
    }
    if (j["tile"].is_object()) {
        m.has_tile = true;
        m.tile_title = j["tile"]["title"].is_null() ? m.name : LocalizedString::from(j["tile"]["title"], m.id);
        m.tile_icon = j["tile"]["icon"].as_string("plugin");
    }
    const Json& settings = j["settings"];
    if (settings.as_bool() || settings.is_object()) {
        m.has_settings = true;
        m.settings_title = settings["title"].is_null() ? m.name : LocalizedString::from(settings["title"], m.id);
    }
    if (!valid_id(m.id)) {
        log::warn("plugins: %s: invalid id", dir.c_str());
        return false;
    }
    if (m.exec.empty() || m.exec[0] == '/' || m.exec.find("..") != std::string::npos ||
        m.exec.find('/') != std::string::npos) {
        log::warn("plugins: %s: bad exec", m.id.c_str());
        return false;
    }
    if (!m.compatible())
        log::warn("plugins: %s: built for api %d (SDK %s), this Facet speaks api %d", m.id.c_str(), m.api,
                  m.sdk.empty() ? "unknown" : m.sdk.c_str(), sdk::kApiVersion);
    return true;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

Json string_array(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Disabled: return "disabled";
        case State::Blocked: return "not started";
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

bool Manifest::compatible() const { return api == sdk::kApiVersion; }

const PermissionRequest* Manifest::permission(const std::string& name) const {
    for (const auto& r : permissions)
        if (r.name == name) return &r;
    return nullptr;
}

bool Plugin::holds(const std::string& perm) const {
    return std::find(granted.begin(), granted.end(), perm) != granted.end() ||
           std::find(transient.begin(), transient.end(), perm) != transient.end();
}

const SurfaceBuffer* Plugin::surface(const std::string& id) const {
    auto it = surfaces.find(id);
    return it == surfaces.end() || it->second.current < 0 ? nullptr : &it->second;
}

bool Manifest::provides_cap(const std::string& cap) const {
    return std::any_of(provides.begin(), provides.end(), [&](const Capability& c) { return c.name == cap; });
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
    if (!sandbox::available())
        log::warn("plugins: containers unavailable (%s); plugins run as plain processes",
                  getuid() == 0 ? "FACET_SANDBOX=0" : "Facet does not run as root");
    refresh_blocks(now);
    for (auto& p : plugins_)
        if (is_enabled(p->m.id) && p->block == Block::None) spawn(*p, now);
}

// ------------------------------------------------------------------ permissions and dependencies

namespace {
// Development and tests: FACET_AUTO_GRANT=1 grants every requested permission.
bool auto_grant() {
    const char* e = getenv("FACET_AUTO_GRANT");
    return e && std::strcmp(e, "1") == 0;
}
}  // namespace

Grant PluginHost::grant(const std::string& id, const std::string& perm) const {
    return grant_from(config_.get("permissions")[id][perm].str());
}

Grant PluginHost::default_grant(const PermissionRequest& r) {
    const PermissionInfo* info = permission_info(r.name);
    if (info && info->level == Level::Normal) return Grant::Allow;
    if (r.transient) return Grant::Ask;
    return r.optional ? Grant::Deny : Grant::Allow;
}

Grant PluginHost::shown_grant(const std::string& id, const PermissionRequest& r) const {
    if (auto_grant()) return Grant::Allow;
    Grant g = grant(id, r.name);
    return g == Grant::Unset ? default_grant(r) : g;
}

std::vector<std::string> PluginHost::start_permissions(const Plugin& p) const {
    std::vector<std::string> out;
    for (const auto& r : p.m.permissions) {
        const PermissionInfo* info = permission_info(r.name);
        if (r.transient || !info) continue;  // transient: only while in use; unknown: never
        Grant g = auto_grant() ? Grant::Allow : grant(p.m.id, r.name);
        if (g == Grant::Unset && info->level == Level::Normal) g = Grant::Allow;
        if (g == Grant::Allow) out.push_back(r.name);
    }
    return out;
}

void PluginHost::set_grant(const std::string& id, const std::string& perm, Grant g, double now) {
    Plugin* p = find(id);
    if (!p) return;
    const PermissionRequest* r = p->m.permission(perm);
    if (!r) return;
    Json all = config_.get("permissions");
    if (!all.is_object()) all = Json::object();
    if (!all[id].is_object()) all[id] = Json::object();
    all[id][perm] = grant_name(g);
    config_.set("permissions", all);
    log::info("plugins: %s: %s set to %s", id.c_str(), perm.c_str(), grant_name(g));
    if (r->transient) {
        if (g == Grant::Deny && p->holds(perm)) revoke_transient(id, perm, now);
    } else if (p->pid > 0 && p->block == Block::None && start_permissions(*p) != p->granted) {
        restart(id, now);  // a running process keeps what it got at start
    }
    changed_ = true;
    refresh_blocks(now);
}

void PluginHost::confirm_permissions(const std::string& id, double now) {
    Plugin* p = find(id);
    if (!p) return;
    Json all = config_.get("permissions");
    if (!all.is_object()) all = Json::object();
    if (!all[id].is_object()) all[id] = Json::object();
    for (const auto& r : p->m.permissions)
        if (grant(id, r.name) == Grant::Unset) all[id][r.name] = grant_name(default_grant(r));
    config_.set("permissions", all);
    refresh_blocks(now);
}

int PluginHost::attention_count() const {
    int n = 0;
    for (const auto& p : plugins_)
        if (is_enabled(p->m.id) && (p->block != Block::None || p->state == State::Failed)) ++n;
    return n;
}

bool PluginHost::sandbox_active() const { return sandbox::available(); }

uid_t PluginHost::plugin_uid(const std::string& id) {
    constexpr int kFirstUid = 64000;  // above login users, below systemd's dynamic range
    Json uids = config_.get("plugin_uids");
    if (!uids.is_object()) uids = Json::object();
    if (uids[id].is_number()) return uid_t(uids[id].as_int());
    int next = kFirstUid;
    for (const auto& [k, v] : uids.fields()) next = std::max(next, v.as_int() + 1);
    uids[id] = next;
    config_.set("plugin_uids", uids);
    return uid_t(next);
}

void PluginHost::refresh_blocks(double now) {
    for (auto& ptr : plugins_) {
        Plugin& p = *ptr;
        Block block = Block::None;
        i18n::Text reason;
        if (!p.m.compatible()) {
            block = Block::Incompatible;
            reason = p.m.api < sdk::kApiVersion
                         ? i18n::Text{"Made for an older Facet (SDK {}). Disabled until the module is updated.",
                                      {p.m.sdk.empty() ? "≤ 0.2" : p.m.sdk}}
                         : i18n::Text{"Made for a newer Facet (SDK {}). Update Facet to use it.",
                                      {p.m.sdk.empty() ? "?" : p.m.sdk}};
        } else if (!auto_grant() && std::any_of(p.m.permissions.begin(), p.m.permissions.end(), [&](const auto& r) {
                       const PermissionInfo* info = permission_info(r.name);
                       return info && info->level != Level::Normal && grant(p.m.id, r.name) == Grant::Unset;
                   })) {
            block = Block::NeedsReview;
            reason = "Waiting for you to review its permissions.";
        } else if (auto denied = std::find_if(p.m.permissions.begin(), p.m.permissions.end(),
                                              [&](const auto& r) {
                                                  return !r.optional && shown_grant(p.m.id, r) == Grant::Deny;
                                              });
                   denied != p.m.permissions.end()) {
            block = Block::MissingPermission;
            reason = {"Needs the “{}” permission.", {denied->name}};
        } else {
            for (const auto& need : p.m.needs) {
                if (!provider_for(p, need)) {
                    block = Block::MissingDependency;
                    reason = {"Needs “{}”, which no installed module provides.", {need.str(), need.install}};
                    break;
                }
            }
        }
        if (block == p.block && reason.key == p.block_reason.key && reason.args == p.block_reason.args) continue;
        p.block = block;
        p.block_reason = reason;
        changed_ = true;
        if (block != Block::None) {
            if (p.pid > 0) kill_now(p);
            p.state = State::Blocked;
            p.display.reset();
            p.ui = Json();
            log::warn("plugins: %s not started: %s", p.m.id.c_str(), i18n::format(reason.key, reason.args).c_str());
        } else if (p.state == State::Blocked) {
            p.state = State::Disabled;
            if (is_enabled(p.m.id)) spawn(p, now);
        }
    }
}

Plugin* PluginHost::provider_for(const Plugin& consumer, const Capability& need) const {
    for (const auto& q : plugins_)
        if (q.get() != &consumer && q->m.compatible() && is_enabled(q->m.id) &&
            std::any_of(q->m.provides.begin(), q->m.provides.end(),
                        [&](const Capability& c) { return c.name == need.name && c.version >= need.version; }))
            return q.get();
    return nullptr;
}

bool PluginHost::all_settled() const {
    return std::none_of(plugins_.begin(), plugins_.end(), [](const auto& p) { return p->state == State::Starting; });
}

// ------------------------------------------------------------------ process management

void PluginHost::spawn(Plugin& p, double now) {
    if (p.block != Block::None) {
        p.state = State::Blocked;
        changed_ = true;
        return;
    }
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
    reset_runtime(p);
    p.granted = start_permissions(p);
    p.sandboxed = sandbox::available();
    p.hidden_since = now;

    sandbox::Spec spec;
    if (p.sandboxed) {
        spec.id = p.m.id;
        spec.plugin_dir = p.m.dir;
        spec.exec = p.m.exec;
        spec.data_dir = data_dir;
        spec.uid = plugin_uid(p.m.id);
        spec.granted = p.granted;
        std::string err = sandbox::prepare(spec);
        if (!err.empty()) {
            log::error("plugins: %s: container: %s", p.m.id.c_str(), err.c_str());
            crash(p, "could not prepare its container", now);
            return;
        }
    }

    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) {
        crash(p, {"pipe failed: {}", {std::strerror(errno)}}, now);
        return;
    }

    pid_t pid;
    std::string plugin_data = p.sandboxed ? "/data" : data_dir;
    // Shared memory for surfaces: optional, a plugin without it still runs.
    bool surfaces_ok = contains(p.granted, sandbox::kSurface) &&
                       sandbox::prepare_surface_dir(p.m.id, p.sandboxed ? spec.uid : 0);
    if (!surfaces_ok && contains(p.granted, sandbox::kSurface))
        log::warn("plugins: %s: no surface directory", p.m.id.c_str());
    if (p.sandboxed && surfaces_ok) spec.surface_dir = sandbox::surface_dir(p.m.id);
    std::string plugin_surfaces = !surfaces_ok          ? std::string()
                                  : p.sandboxed         ? std::string(sandbox::kSurfaceDirInContainer)
                                                        : sandbox::surface_dir(p.m.id);
    // Capability endpoints: the provider's directory per capability and, for
    // each capability this plugin requires, its own subdirectory of the provider's.
    Json endpoints = Json::object();
    endpoints["provides"] = Json::object();
    endpoints["requires"] = Json::object();
    for (const auto& c : p.m.provides) {
        if (!c.endpoint) continue;
        std::string host = sandbox::endpoint_dir(p.m.id, c.name);
        if (!sandbox::prepare_endpoint_dir(host, p.sandboxed ? spec.uid : 0)) {
            log::warn("plugins: %s: no endpoint directory for %s", p.m.id.c_str(), c.name.c_str());
            continue;
        }
        std::string inside = std::string(sandbox::kProvidesInContainer) + "/" + c.name;
        if (p.sandboxed) spec.binds.push_back({host, inside});
        endpoints["provides"][c.name] = p.sandboxed ? inside : host;
    }
    for (const auto& need : p.m.needs) {
        Plugin* prov = provider_for(p, need);
        if (!prov || std::none_of(prov->m.provides.begin(), prov->m.provides.end(), [&](const Capability& c) {
                return c.name == need.name && c.endpoint;
            }))
            continue;
        // Owned by the provider: it creates the socket (or whatever it shares) there.
        uid_t owner = p.sandboxed ? plugin_uid(prov->m.id) : 0;
        std::string host = sandbox::endpoint_dir(prov->m.id, need.name, p.m.id);
        if (!sandbox::prepare_endpoint_dir(sandbox::endpoint_dir(prov->m.id, need.name), owner) ||
            !sandbox::prepare_endpoint_dir(host, owner)) {
            log::warn("plugins: %s: no endpoint directory for %s", p.m.id.c_str(), need.name.c_str());
            continue;
        }
        std::string inside = std::string(sandbox::kRequiresInContainer) + "/" + need.name;
        if (p.sandboxed) spec.binds.push_back({host, inside});
        endpoints["requires"][need.name] = p.sandboxed ? inside : host;
    }
    if (p.sandboxed) {
        // A clean environment: nothing of Facet's leaks into the container.
        spec.env = {"FACET_PLUGIN_ID=" + p.m.id, "FACET_PLUGIN_DATA=/data",
                    "FACET_API=" + std::to_string(sdk::kApiVersion), "HOME=/data", "TMPDIR=/tmp",
                    "PATH=/plugin:/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8"};
        pid = sandbox::spawn(spec, in[0], out[1], err[1]);
    } else {
        // Everything the child needs is prepared before fork (no allocation after).
        std::vector<std::string> env_store;
        for (char** e = environ; *e; ++e) {
            if (std::strncmp(*e, "FACET_PLUGIN_", 13) == 0) continue;
            env_store.emplace_back(*e);
        }
        env_store.push_back("FACET_PLUGIN_ID=" + p.m.id);
        env_store.push_back("FACET_PLUGIN_DATA=" + data_dir);
        env_store.push_back("FACET_API=" + std::to_string(sdk::kApiVersion));
        std::vector<char*> envp;
        for (auto& e : env_store) envp.push_back(e.data());
        envp.push_back(nullptr);
        char* argv[] = {exe.data(), nullptr};
        pid_t parent = getpid();

        pid = fork();
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
    }
    if (pid < 0) {
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) ::close(fd);
        crash(p, {"fork failed: {}", {std::strerror(errno)}}, now);
        return;
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
    if (p.sandboxed) {
        std::string perms;
        for (const auto& g : p.granted) perms += (perms.empty() ? "" : ", ") + g;
        log::info("plugins: started %s in a container (pid %d, uid %d, permissions: %s)", p.m.id.c_str(), pid,
                  int(spec.uid), perms.empty() ? "none" : perms.c_str());
    } else {
        log::info("plugins: started %s (pid %d)", p.m.id.c_str(), pid);
    }

    Json hello = Json::object();
    hello["t"] = "hello";
    hello["api"] = sdk::kApiVersion;
    hello["data_dir"] = plugin_data;
    hello["permissions"] = string_array(p.granted);
    hello["surface_dir"] = plugin_surfaces;
    hello["screen"]["w"] = screen_w_;
    hello["screen"]["h"] = screen_h_;
    hello["theme"] = theme_;
    hello["locale"] = locale_;
    hello["timezone"] = timezone_;
    hello["content_width"] = content_width_;
    hello["endpoints"] = endpoints;
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

// Everything a process held ends with it (fail-safe).
void PluginHost::reset_runtime(Plugin& p) {
    if (!p.transient.empty() || p.wake_lock || p.subscribed || !p.background_task.empty()) changed_ = true;
    tell_compositor(p, false);
    tell_providers(p, false);
    p.inset_bottom = -1;
    if (compositor() == &p || (p.m.provides_cap("display.wayland") && !wayland_clients_.empty())) {
        wayland_clients_.clear();
        changed_ = true;
    }
    unmap_surfaces(p);
    p.text_input = false;
    p.transient.clear();
    p.revoke_deadline.clear();
    p.frozen = false;
    p.wake_lock = false;
    p.subscribed = false;
    p.background_task.clear();
    p.badge.clear();
    p.tile_icon_name.clear();
    p.tile_icon_ops = Json();
    prompts_.erase(std::remove_if(prompts_.begin(), prompts_.end(),
                                  [&](const PermissionPrompt& q) { return q.plugin == p.m.id; }),
                   prompts_.end());
    notifications_.remove_calls_of(p.m.id);  // nobody could answer them
}

void PluginHost::kill_now(Plugin& p) {
    if (p.pid <= 0) return;
    if (p.frozen) sandbox::signal_all(p.pid, p.sandboxed, SIGCONT);
    ::kill(p.pid, SIGKILL);
    int st = 0;
    while (waitpid(p.pid, &st, 0) < 0 && errno == EINTR) {
    }
    p.pid = -1;
    close_fds(p);
    reset_runtime(p);
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
    reset_runtime(p);
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
    reset_runtime(p);
    if (p.state == State::Stopping) {
        close_fds(p);
        p.state = State::Disabled;
        p.display.reset();
        p.ui = Json();
        changed_ = true;
        log::info("plugins: %s stopped", p.m.id.c_str());
        return;
    }
    if (p.sandboxed && WIFEXITED(status) && WEXITSTATUS(status) == sandbox::kSetupFailed)
        crash(p, "could not start its container (details in the log)", now);
    else
        crash(p, "process exited", now);
    p.error.wait_status = status;  // already reaped, so crash() could not see it
}

void PluginHost::shutdown_all() {
    Json bye = Json::object();
    bye["t"] = "shutdown";
    bool any = false;
    for (auto& p : plugins_) {
        if (p->pid <= 0) continue;
        if (p->frozen) sandbox::signal_all(p->pid, p->sandboxed, SIGCONT);
        p->frozen = false;
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
        if (msg["api"].as_int() != sdk::kApiVersion || msg["id"].str() != p.m.id) {
            crash(p, "invalid hello", now);
            return;
        }
        p.sdk_reported = msg["sdk"].str();
        p.state = State::Running;
        p.error = {};
        p.last_pong = now;
        p.next_ping = now + kPingInterval;
        changed_ = true;
        if (compositor() == &p) {
            for (auto& q : plugins_)
                if (q.get() != &p && q->state == State::Running) tell_compositor(*q, true);
        } else {
            tell_compositor(p, true);
        }
        tell_providers(p, true);
        tell_consumers(p);
        tell_lent(p);
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
    } else if (t == "keyboard_ui" || t == "input") {
        if (!p.m.provides_cap("input.keyboard")) {
            log::warn("plugins: %s: keyboard message, but it does not provide input.keyboard", p.m.id.c_str());
            return;
        }
        if (t == "keyboard_ui") {
            if (msg["ops"].is_array()) {
                p.keyboard_ops = msg["ops"];
                p.keyboard_height = std::clamp(float(msg["height"].as_number(0)), 0.f, 1000.f);
                changed_ = true;
            }
        } else {
            const std::string& a = msg["action"].str();
            if (a == "insert" || a == "backspace" || a == "enter" || a == "hide") {
                input_.push_back({p.m.id, a, msg["text"].str()});
                changed_ = true;
            }
        }
    } else if (t == "surface" || t == "surface_frame" || t == "surface_destroy") {
        if (!p.holds(sandbox::kSurface)) {
            log::warn("plugins: %s: surface without the display.surface permission", p.m.id.c_str());
            return;
        }
        handle_surface(p, msg);
    } else if (t == "text_input") {
        if (!p.holds(sandbox::kSurface)) return;
        p.text_input = msg["active"].as_bool();
        p.text_mode = msg["mode"].str() == "number" ? "number" : "text";
        changed_ = true;
    } else if (t == "badge") {
        std::string b = msg["value"].str();
        if (b.size() > 8) b.resize(8);
        if (b != p.badge) p.badge = b, changed_ = true;
    } else if (t == "tile_icon") {
        p.tile_icon_name = msg["name"].str();
        p.tile_icon_ops = msg["ops"].is_array() && msg["ops"].size() <= 200 ? msg["ops"] : Json();
        changed_ = true;
    } else if (t == "permission_request") {
        handle_permission_request(p, msg["name"].str(), msg["reason"].str(), now);
    } else if (t == "permission_release") {
        const std::string& perm = msg["name"].str();
        if (std::find(p.transient.begin(), p.transient.end(), perm) != p.transient.end()) {
            drop_transient(p, perm);
            log::info("plugins: %s released %s", p.m.id.c_str(), perm.c_str());
        }
        p.revoke_deadline.erase(perm);
        changed_ = true;
    } else if (t == "notify") {
        handle_notify(p, msg, now);
    } else if (t == "notify_cancel") {
        std::string source = msg["source"].as_string(p.m.id);
        if (source != p.m.id && !p.holds(sandbox::kDistributor)) return;
        if (notifications_.cancel(p.m.id, source, msg["id"].str())) changed_ = true;
    } else if (t == "notifications_subscribe") {
        if (!p.holds(sandbox::kDistributor)) {
            log::warn("plugins: %s: subscribe without the notifications.distributor permission", p.m.id.c_str());
            return;
        }
        p.subscribed = msg["value"].as_bool(true);
    } else if (t == "background") {
        if (!p.holds(sandbox::kBackground)) return;  // it is paused in the background anyway
        std::string reason = msg["value"].as_bool() ? msg["reason"].str() : std::string();
        if (reason.size() > 120) reason.resize(120);
        if (msg["value"].as_bool() && reason.empty()) reason = " ";  // working, no reason given
        p.background_task = reason;
        changed_ = true;
    } else if (t == "wake_lock") {
        if (!p.holds(sandbox::kWakeLock)) {
            log::warn("plugins: %s: wake_lock without the permission", p.m.id.c_str());
            return;
        }
        p.wake_lock = msg["value"].as_bool();
        changed_ = true;
    } else if (t == "wayland_clients") {
        if (compositor() != &p) {
            log::warn("plugins: %s: wayland_clients from a module that is not the compositor", p.m.id.c_str());
            return;
        }
        wayland_clients_.clear();
        for (const auto& c : msg["clients"].items()) {
            if (wayland_clients_.size() >= 64) break;
            wayland_clients_.push_back({c["module"].str(), c["app_id"].str(), c["title"].str(), c["pid"].as_int(),
                                        c["focused"].as_bool()});
        }
        changed_ = true;
    } else if (t == "display") {
        if (!contains(p.granted, sandbox::kDisplayPower)) {
            log::warn("plugins: %s: display request without the display.power permission", p.m.id.c_str());
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
                if (p.frozen) break;  // stopped on purpose: no pings
                for (auto it = p.revoke_deadline.begin(); it != p.revoke_deadline.end(); ++it) {
                    if (now < it->second) continue;
                    log::warn("plugins: %s kept %s after it was withdrawn; restarting it", p.m.id.c_str(),
                              it->first.c_str());
                    restart(p.m.id, now);
                    break;
                }
                if (p.pid <= 0) break;
                if (!p.visible && (p.m.has_tile || p.m.has_settings) && !p.holds(sandbox::kBackground) &&
                    p.transient.empty() && now - p.hidden_since > kFreezeAfter) {
                    freeze(p, true, now);
                    break;
                }
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
            if (p.wbuf.size() > kMaxOutbox && !p.frozen) crash(p, "not reading messages", now);
        }
    }
    for (const auto& n : notifications_.expire_calls(now)) {
        if (Plugin* poster = find(n.poster); poster && poster->state == State::Running) {
            Json a = Json::object();
            a["t"] = "notification_action";
            a["id"] = n.id;
            a["action"] = "timeout";
            a["source"] = n.source;
            send(*poster, a);
        }
        changed_ = true;
    }
}

double PluginHost::next_deadline(double now) const {
    double t = now + 3600;
    for (const auto& p : plugins_) {
        switch (p->state) {
            case State::Starting:
            case State::Stopping: t = std::min(t, p->deadline); break;
            case State::Running:
                if (p->frozen) break;
                t = std::min({t, p->next_ping, p->last_pong + kPongTimeout});
                if (!p->visible && !p->holds(sandbox::kBackground)) t = std::min(t, p->hidden_since + kFreezeAfter + 0.1);
                for (const auto& [perm, at] : p->revoke_deadline) t = std::min(t, at);
                break;
            case State::Backoff: t = std::min(t, p->restart_at); break;
            default: break;
        }
    }
    for (const auto& n : notifications_.list())
        if (n.is_call()) t = std::min(t, n.ring_until);  // calls stop ringing on time
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
        if (p->state == State::Disabled || p->state == State::Failed || p->state == State::Blocked) {
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
    refresh_blocks(now);  // plugins that depend on this one
}

void PluginHost::restart(const std::string& id, double now) {
    Plugin* p = find(id);
    if (!p || !is_enabled(id) || p->block != Block::None) return;
    kill_now(*p);
    p->crashes.clear();
    p->error = {};
    spawn(*p, now);
}

void PluginHost::set_visible(const std::string& id, bool visible) {
    Plugin* p = find(id);
    if (!p || p->visible == visible) return;
    p->visible = visible;
    p->hidden_since = mono_now();
    if (p->state != State::Running) return;
    if (visible && p->frozen) freeze(*p, false, mono_now());
    Json msg = Json::object();
    msg["t"] = "visible";
    msg["value"] = visible;
    send(*p, msg);
    tell_providers(*p, true);  // e.g. the compositor renders only what is on screen
}

void PluginHost::send_event(const std::string& id, const std::string& widget, const Json& value,
                            const std::string& action) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "event";
    msg["id"] = widget;
    msg["value"] = value;
    if (!action.empty()) msg["action"] = action;
    send(*p, msg);
}

void PluginHost::broadcast_activity() {
    Json msg = Json::object();
    msg["t"] = "activity";
    for (auto& p : plugins_)
        if (p->state == State::Running && contains(p->granted, sandbox::kDisplayPower)) send(*p, msg);
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

void PluginHost::set_screen_size(int w, int h) {
    if (w == screen_w_ && h == screen_h_) return;
    screen_w_ = w;
    screen_h_ = h;
    Json msg = Json::object();
    msg["t"] = "layout";
    msg["content_width"] = content_width_;
    msg["screen"]["w"] = w;
    msg["screen"]["h"] = h;
    for (auto& p : plugins_)
        if (p->state == State::Running) send(*p, msg);
}

void PluginHost::send_touch(const std::string& id, const std::string& surface, const char* kind, float x, float y) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running || p->frozen) return;
    Json msg = Json::object();
    msg["t"] = "touch";
    msg["surface"] = surface;
    msg["kind"] = kind;
    msg["x"] = std::round(x * 10) / 10;
    msg["y"] = std::round(y * 10) / 10;
    send(*p, msg);
}

void PluginHost::send_insets(const std::string& id, int bottom) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running || p->inset_bottom == bottom) return;
    p->inset_bottom = bottom;
    Json msg = Json::object();
    msg["t"] = "insets";
    msg["bottom"] = bottom;
    send(*p, msg);
}

void PluginHost::send_text(const std::string& id, const std::string& action, const std::string& text) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "text";
    msg["action"] = action;
    if (!text.empty()) msg["text"] = text;
    send(*p, msg);
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

bool PluginHost::keyboard_ready(const std::string& id) const {
    for (const auto& p : plugins_)
        if (p->m.id == id)
            return p->state == State::Running && p->m.provides_cap("input.keyboard") && p->keyboard_height > 0;
    return false;
}

void PluginHost::keyboard_show(const std::string& id, const std::string& mode, float width,
                               const std::vector<std::string>& langs) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running || !p->m.provides_cap("input.keyboard")) return;
    p->keyboard_ops = Json();
    p->keyboard_height = 0;
    Json msg = Json::object();
    msg["t"] = "keyboard_show";
    msg["mode"] = mode;
    msg["width"] = width;
    Json l = Json::array();
    for (const auto& s : langs) l.push_back(s);
    msg["langs"] = l;
    send(*p, msg);
}

void PluginHost::keyboard_key(const std::string& id, const std::string& hit) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "keyboard_key";
    msg["hit"] = hit;
    send(*p, msg);
}

void PluginHost::keyboard_hide(const std::string& id) {
    Plugin* p = find(id);
    if (!p || p->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "keyboard_hide";
    send(*p, msg);
}

std::vector<PluginHost::InputAction> PluginHost::take_input() {
    std::vector<InputAction> out;
    out.swap(input_);
    return out;
}

std::optional<bool> PluginHost::display_policy(double now) {
    if (now < wake_until_ || notifications_.ringing(now)) return true;
    for (const auto& p : plugins_)
        if (p->state == State::Running && !p->frozen && p->wake_lock) return true;
    for (const auto& p : plugins_)
        if (p->state == State::Running && contains(p->granted, sandbox::kDisplayPower) && p->display)
            return p->display;
    return std::nullopt;
}

// ------------------------------------------------------------------ transient permissions

void PluginHost::handle_permission_request(Plugin& p, const std::string& perm, const std::string& reason,
                                           double now) {
    const PermissionRequest* r = p.m.permission(perm);
    if (!r || !r->transient) {
        // Persistent ones are decided in Settings; answer with what it has.
        Json reply = Json::object();
        reply["t"] = "permission";
        reply["name"] = perm;
        reply["granted"] = p.holds(perm);
        send(p, reply);
        return;
    }
    if (p.holds(perm)) {
        grant_transient(p, perm, true, now);
        return;
    }
    Grant g = auto_grant() ? Grant::Allow : grant(p.m.id, perm);
    if (g == Grant::Allow) {
        grant_transient(p, perm, true, now);
    } else if (g == Grant::Deny) {
        grant_transient(p, perm, false, now);
    } else {
        bool queued = std::any_of(prompts_.begin(), prompts_.end(),
                                  [&](const PermissionPrompt& q) { return q.plugin == p.m.id && q.permission == perm; });
        std::string why = reason.size() > 200 ? reason.substr(0, 200) : reason;
        if (!queued) prompts_.push_back({p.m.id, perm, why});
        wake_until_ = std::max(wake_until_, now + kNotificationWake);
        changed_ = true;
    }
}

void PluginHost::grant_transient(Plugin& p, const std::string& perm, bool granted, double now) {
    (void)now;
    if (granted && !p.holds(perm)) {
        if (p.sandboxed && p.pid > 0) {
            std::string err = sandbox::attach_devices(p.pid, plugin_uid(p.m.id), perm);
            if (!err.empty()) {
                log::error("plugins: %s: cannot give %s: %s", p.m.id.c_str(), perm.c_str(), err.c_str());
                granted = false;
            }
        }
        if (granted) {
            p.transient.push_back(perm);
            log::info("plugins: %s: %s granted while in use", p.m.id.c_str(), perm.c_str());
        }
    }
    Json reply = Json::object();
    reply["t"] = "permission";
    reply["name"] = perm;
    reply["granted"] = granted;
    send(p, reply);
    changed_ = true;
}

void PluginHost::drop_transient(Plugin& p, const std::string& perm) {
    p.transient.erase(std::remove(p.transient.begin(), p.transient.end(), perm), p.transient.end());
    if (p.sandboxed && p.pid > 0) {
        std::string err = sandbox::detach_devices(p.pid, perm);
        if (!err.empty()) log::warn("plugins: %s: removing %s: %s", p.m.id.c_str(), perm.c_str(), err.c_str());
    }
}

const PermissionPrompt* PluginHost::pending_prompt() const { return prompts_.empty() ? nullptr : &prompts_.front(); }

void PluginHost::answer_prompt(bool allow, bool always, double now) {
    if (prompts_.empty()) return;
    PermissionPrompt q = prompts_.front();
    prompts_.pop_front();
    changed_ = true;
    if (always) set_grant(q.plugin, q.permission, allow ? Grant::Allow : Grant::Deny, now);
    Plugin* p = find(q.plugin);
    if (p && p->state == State::Running) grant_transient(*p, q.permission, allow, now);
}

void PluginHost::revoke_transient(const std::string& id, const std::string& perm, double now) {
    Plugin* p = find(id);
    if (!p || std::find(p->transient.begin(), p->transient.end(), perm) == p->transient.end()) return;
    drop_transient(*p, perm);  // new opens fail at once
    Json msg = Json::object();
    msg["t"] = "permission";
    msg["name"] = perm;
    msg["granted"] = false;
    send(*p, msg);
    p->revoke_deadline[perm] = now + kRevokeGrace;  // and it must close what it has open
    log::info("plugins: %s: %s withdrawn by the user", id.c_str(), perm.c_str());
    changed_ = true;
}

// ------------------------------------------------------------------ notifications

void PluginHost::handle_notify(Plugin& p, const Json& msg, double now) {
    Notification n = Notification::from_json(msg["notification"]);
    n.poster = p.m.id;
    bool on_behalf = (!n.source.empty() && n.source != p.m.id) || !n.app_name.empty();
    if (on_behalf ? !p.holds(sandbox::kDistributor) : !p.holds(sandbox::kNotifications)) {
        log::warn("plugins: %s: notification without the %s permission", p.m.id.c_str(),
                  on_behalf ? sandbox::kDistributor : sandbox::kNotifications);
        return;
    }
    if (n.source.empty()) n.source = p.m.id;
    if (n.id.empty()) return;
    const Notification& stored = notifications_.post(std::move(n), now);
    if (stored.is_call() || stored.priority == "high")
        wake_until_ = std::max(wake_until_, now + (stored.is_call() ? 0.0 : kNotificationWake));
    Json copy = Json::object();
    copy["t"] = "notification_posted";
    copy["notification"] = stored.to_json();
    for (auto& d : plugins_)
        if (d.get() != &p && d->subscribed && d->state == State::Running && d->holds(sandbox::kDistributor))
            send(*d, copy);
    changed_ = true;
}

void PluginHost::notification_action(uint64_t serial, const std::string& action, double now) {
    std::optional<Notification> n = notifications_.take(serial);
    if (!n) return;
    changed_ = true;
    Plugin* poster = find(n->poster);
    if (!poster || poster->state != State::Running) return;
    if (poster->frozen) freeze(*poster, false, now);  // let it react (e.g. open the chat)
    Json msg = Json::object();
    msg["t"] = "notification_action";
    msg["id"] = n->id;
    msg["action"] = action;
    msg["source"] = n->source;
    send(*poster, msg);
}

// ------------------------------------------------------------------ surfaces

namespace {
constexpr size_t kMaxSurfaces = 8;  // own and lent

bool valid_surface_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
}  // namespace

void PluginHost::unmap_surfaces(Plugin& p) {
    for (auto& [id, s] : p.surfaces) {
        if (s.map) munmap(const_cast<uint8_t*>(s.map), s.size);
        if (!s.lent_to.empty()) send_lent(p, id, s.lent_to, nullptr);
    }
    if (!p.surfaces.empty()) changed_ = true;
    p.surfaces.clear();
}

// The plugin's shared memory is only ever read, and only within the size
// checked here, so a misbehaving plugin can at worst show garbage.
void PluginHost::handle_surface(Plugin& p, const Json& msg) {
    const std::string& t = msg["t"].str();
    const std::string id = msg["id"].str();
    if (!valid_surface_id(id)) return;
    auto it = p.surfaces.find(id);
    if (t == "surface_destroy") {
        if (it != p.surfaces.end()) {
            if (it->second.map) munmap(const_cast<uint8_t*>(it->second.map), it->second.size);
            if (!it->second.lent_to.empty()) send_lent(p, id, it->second.lent_to, nullptr);
            p.surfaces.erase(it);
            changed_ = true;
        }
        return;
    }
    if (t == "surface_frame") {
        if (it == p.surfaces.end()) return;
        int b = msg["buffer"].as_int(-1);
        if (b < 0 || b >= it->second.buffers) return;
        it->second.current = b;
        Json ack = Json::object();
        ack["t"] = "surface_shown";
        ack["id"] = id;
        ack["buffer"] = b;
        send(p, ack);
        changed_ = true;
        return;
    }
    // "surface": create or replace.
    int w = msg["w"].as_int(), h = msg["h"].as_int(), stride = msg["stride"].as_int(), buffers = msg["buffers"].as_int();
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || stride != w * 4 || buffers < 1 || buffers > 3) return;
    if (it == p.surfaces.end() && p.surfaces.size() >= kMaxSurfaces) return;
    // Lent to a module that requires a capability of this one (a compositor
    // shows each app's windows on that app's screen).
    const std::string lent_to = msg["for"].str();
    if (!lent_to.empty()) {
        Plugin* q = find(lent_to);
        if (!q || q == &p || std::none_of(q->m.needs.begin(), q->m.needs.end(),
                                          [&](const Capability& c) { return provider_for(*q, c) == &p; })) {
            log::warn("plugins: %s: surface %s for %s, which does not depend on it", p.m.id.c_str(), id.c_str(),
                      lent_to.c_str());
            return;
        }
    }
    std::string path = sandbox::surface_dir(p.m.id) + "/" + id + ".buf";
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        log::warn("plugins: %s: surface %s: cannot open its buffer", p.m.id.c_str(), id.c_str());
        return;
    }
    struct stat st;
    size_t need = size_t(stride) * size_t(h) * size_t(buffers);
    void* map = MAP_FAILED;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && size_t(st.st_size) >= need)
        map = mmap(nullptr, need, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        log::warn("plugins: %s: surface %s: buffer too small or not mappable", p.m.id.c_str(), id.c_str());
        return;
    }
    if (it != p.surfaces.end()) {
        if (it->second.map) munmap(const_cast<uint8_t*>(it->second.map), it->second.size);
        if (it->second.lent_to != lent_to && !it->second.lent_to.empty()) send_lent(p, id, it->second.lent_to, nullptr);
    }
    SurfaceBuffer& s = p.surfaces[id];
    s = SurfaceBuffer{w, h, stride, buffers, -1, static_cast<const uint8_t*>(map), need, lent_to};
    if (!lent_to.empty()) send_lent(p, id, lent_to, &s);
    changed_ = true;
}

SurfaceRef PluginHost::find_surface(const Plugin& viewer, const std::string& id) const {
    if (const SurfaceBuffer* own = viewer.surface(id)) return {&viewer, own};
    for (const auto& q : plugins_) {
        if (q.get() == &viewer || q->state != State::Running) continue;
        const SurfaceBuffer* s = q->surface(id);
        if (s && s->lent_to == viewer.m.id) return {q.get(), s};
    }
    return {};
}

void PluginHost::send_lent(const Plugin& owner, const std::string& id, const std::string& to, const SurfaceBuffer* s) {
    Plugin* q = find(to);
    if (!q || q->state != State::Running) return;
    Json msg = Json::object();
    msg["t"] = "surface_lent";
    msg["id"] = id;
    msg["from"] = owner.m.id;
    msg["available"] = s != nullptr;
    if (s) {
        msg["w"] = s->w;
        msg["h"] = s->h;
    }
    send(*q, msg);
}

void PluginHost::tell_lent(Plugin& consumer) {
    for (const auto& q : plugins_)
        if (q.get() != &consumer && q->state == State::Running)
            for (const auto& [id, s] : q->surfaces)
                if (s.lent_to == consumer.m.id) send_lent(*q, id, s.lent_to, &s);
}

// ------------------------------------------------------------------ background

void PluginHost::freeze(Plugin& p, bool on, double now) {
    if (p.frozen == on || p.pid <= 0) return;
    sandbox::signal_all(p.pid, p.sandboxed, on ? SIGSTOP : SIGCONT);
    p.frozen = on;
    if (!on) {
        p.last_pong = now;  // the clock ran while it was stopped
        p.next_ping = now + kPingInterval;
        p.hidden_since = now;
    }
    log::info("plugins: %s %s", p.m.id.c_str(), on ? "paused (no background permission)" : "resumed");
    changed_ = true;
}

std::vector<BackgroundEntry> PluginHost::background_registry() const {
    std::vector<BackgroundEntry> out;
    for (const auto& p : plugins_)
        if (p->state == State::Running && !p->visible && !p->frozen && p->holds(sandbox::kBackground))
            out.push_back({p->m.id, p->background_task == " " ? std::string() : p->background_task});
    return out;
}

// ------------------------------------------------------------------ Wayland

Plugin* PluginHost::compositor() {
    for (auto& p : plugins_)
        if (p->state == State::Running && p->m.provides_cap("display.wayland") && p->holds(sandbox::kCompositor))
            return p.get();
    return nullptr;
}

// ------------------------------------------------------------------ capability endpoints

namespace {
Json consumer_message(const Plugin& provider, const Plugin& consumer, const Capability& cap, bool running) {
    Json msg = Json::object();
    msg["t"] = "consumer";
    msg["capability"] = cap.name;
    msg["module"] = consumer.m.id;
    msg["running"] = running;
    msg["visible"] = running && consumer.visible;
    if (std::any_of(provider.m.provides.begin(), provider.m.provides.end(),
                    [&](const Capability& c) { return c.name == cap.name && c.endpoint; }))
        msg["dir"] = (provider.sandboxed ? std::string(sandbox::kProvidesInContainer) + "/" + cap.name
                                         : sandbox::endpoint_dir(provider.m.id, cap.name)) +
                     "/" + consumer.m.id;
    return msg;
}
}  // namespace

void PluginHost::tell_providers(Plugin& consumer, bool running) {
    for (const auto& need : consumer.m.needs) {
        Plugin* prov = provider_for(consumer, need);
        if (prov && prov->state == State::Running) send(*prov, consumer_message(*prov, consumer, need, running));
    }
}

void PluginHost::tell_consumers(Plugin& provider) {
    for (auto& q : plugins_) {
        if (q.get() == &provider || q->state != State::Running) continue;
        for (const auto& need : q->m.needs)
            if (provider_for(*q, need) == &provider) send(provider, consumer_message(provider, *q, need, true));
    }
}

// Tells the compositor which wayland.* scopes a client module has. They are
// the client's own grants: nothing of the compositor's permissions carries over.
void PluginHost::tell_compositor(Plugin& client, bool running) {
    bool is_client = std::any_of(client.m.needs.begin(), client.m.needs.end(),
                                 [](const Capability& c) { return c.name == "display.wayland"; });
    if (!is_client) return;
    Plugin* comp = compositor();
    if (!comp || comp == &client) return;
    Json scopes = Json::array();
    for (const auto& g : client.granted)
        if (g.rfind("wayland.", 0) == 0) scopes.push_back(g);
    for (const auto& g : client.transient)
        if (g.rfind("wayland.", 0) == 0) scopes.push_back(g);
    Json msg = Json::object();
    msg["t"] = "wayland_client";
    msg["module"] = client.m.id;
    msg["scopes"] = scopes;
    msg["running"] = running;
    send(*comp, msg);
}

}  // namespace facet::plugins
