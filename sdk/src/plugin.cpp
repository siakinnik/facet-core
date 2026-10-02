#include "facet/plugin.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace facet::sdk {

namespace {
// Same rules as the core: "" = system zone, else an IANA id.
void apply_timezone(const std::string& zone) {
    if (zone.empty()) unsetenv("TZ");
    else setenv("TZ", zone == "UTC" ? "UTC0" : (":" + zone).c_str(), 1);
    tzset();
}
}  // namespace

// ---------------------------------------------------------------- Screen

Screen::Screen(std::string title) {
    root_["title"] = std::move(title);
    root_["items"] = Json::array();
}

Json& Screen::add(const char* type) {
    Json item = Json::object();
    item["type"] = type;
    Json& items = root_["items"];
    items.push_back(std::move(item));
    return items.back();
}

Screen& Screen::section(std::string title) {
    add("section")["title"] = std::move(title);
    return *this;
}

Screen& Screen::info(std::string label, std::string value, std::string tone) {
    Json& j = add("info");
    j["label"] = std::move(label);
    j["value"] = std::move(value);
    j["tone"] = std::move(tone);
    return *this;
}

Screen& Screen::note(std::string text) {
    add("note")["text"] = std::move(text);
    return *this;
}

Screen& Screen::toggle(std::string id, std::string label, bool value) {
    Json& j = add("toggle");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    j["value"] = value;
    return *this;
}

Screen& Screen::select(std::string id, std::string label, std::vector<std::string> options,
                       int value) {
    Json& j = add("select");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    Json opts = Json::array();
    for (auto& o : options) opts.push_back(std::move(o));
    j["options"] = std::move(opts);
    j["value"] = value;
    return *this;
}

Screen& Screen::stepper(std::string id, std::string label, int value, int min, int max, int step,
                        std::string unit) {
    Json& j = add("stepper");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    j["value"] = value;
    j["min"] = min;
    j["max"] = max;
    j["step"] = step;
    j["unit"] = std::move(unit);
    return *this;
}

Screen& Screen::time(std::string id, std::string label, int minutes, int step) {
    Json& j = add("time");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    j["value"] = minutes;
    j["step"] = step;
    return *this;
}

Screen& Screen::level(std::string label, double value, std::string text) {
    Json& j = add("level");
    j["label"] = std::move(label);
    j["value"] = value;
    j["text"] = std::move(text);
    return *this;
}

Screen& Screen::button(std::string id, std::string label, std::string style) {
    Json& j = add("button");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    j["style"] = std::move(style);
    return *this;
}

Screen& Screen::text_field(std::string id, std::string label, std::string value, std::string placeholder,
                           bool secure, std::string mode, int max_length) {
    Json& j = add("text");
    j["id"] = std::move(id);
    j["label"] = std::move(label);
    j["value"] = std::move(value);
    j["placeholder"] = std::move(placeholder);
    j["secure"] = secure;
    j["mode"] = std::move(mode);
    j["max"] = max_length;
    return *this;
}

Screen& Screen::canvas(std::string id, float height, const Canvas& canvas) {
    Json& j = add("canvas");
    j["id"] = std::move(id);
    j["height"] = height;
    j["ops"] = canvas.ops();
    return *this;
}

Screen& Screen::surface(std::string id, float height) {
    Json& j = add("surface");
    j["id"] = std::move(id);
    j["height"] = height;
    return *this;
}

Screen& Screen::fullscreen(std::string surface_id) {
    root_["fullscreen"] = std::move(surface_id);
    return *this;
}

// ---------------------------------------------------------------- Canvas

#ifndef FACET_SDK_VERSION
#define FACET_SDK_VERSION "unknown"  // set by sdk/CMakeLists.txt
#endif

const char* sdk_version() { return FACET_SDK_VERSION; }

Json& Canvas::add(const char* op) {
    Json item = Json::object();
    item["op"] = op;
    ops_.push_back(std::move(item));
    return ops_.back();
}

namespace {
void set_hit(Json& j, std::string hit, std::string pressed) {
    if (!hit.empty()) j["hit"] = std::move(hit);
    if (!pressed.empty()) j["pressed"] = std::move(pressed);
}
}  // namespace

Canvas& Canvas::rect(float x, float y, float w, float h, std::string color, std::string hit, std::string pressed) {
    Json& j = add("rect");
    j["x"] = x, j["y"] = y, j["w"] = w, j["h"] = h;
    j["color"] = std::move(color);
    set_hit(j, std::move(hit), std::move(pressed));
    return *this;
}

Canvas& Canvas::rrect(float x, float y, float w, float h, float radius, std::string color, std::string hit,
                      std::string pressed) {
    Json& j = add("rrect");
    j["x"] = x, j["y"] = y, j["w"] = w, j["h"] = h, j["r"] = radius;
    j["color"] = std::move(color);
    set_hit(j, std::move(hit), std::move(pressed));
    return *this;
}

Canvas& Canvas::circle(float cx, float cy, float r, std::string color, std::string hit, std::string pressed) {
    Json& j = add("circle");
    j["cx"] = cx, j["cy"] = cy, j["r"] = r;
    j["color"] = std::move(color);
    set_hit(j, std::move(hit), std::move(pressed));
    return *this;
}

Canvas& Canvas::ring(float cx, float cy, float r, float width, std::string color) {
    Json& j = add("ring");
    j["cx"] = cx, j["cy"] = cy, j["r"] = r, j["width"] = width;
    j["color"] = std::move(color);
    return *this;
}

Canvas& Canvas::line(float x1, float y1, float x2, float y2, float width, std::string color) {
    Json& j = add("line");
    j["x1"] = x1, j["y1"] = y1, j["x2"] = x2, j["y2"] = y2, j["width"] = width;
    j["color"] = std::move(color);
    return *this;
}

Canvas& Canvas::arc(float cx, float cy, float r, float width, float start, float sweep, std::string color) {
    Json& j = add("arc");
    j["cx"] = cx, j["cy"] = cy, j["r"] = r, j["width"] = width, j["start"] = start, j["sweep"] = sweep;
    j["color"] = std::move(color);
    return *this;
}

namespace {
Json points_json(const std::vector<float>& points) {
    Json pts = Json::array();
    for (float v : points) pts.push_back(Json(std::round(v * 10) / 10));  // 0.1 dp is plenty
    return pts;
}
}  // namespace

// ---------------------------------------------------------------- Surface

bool Surface::create(Plugin& plugin, const std::string& id, int width, int height, const std::string& for_module) {
    destroy();
    if (plugin.surface_dir_.empty() || width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    std::string path = plugin.surface_dir_ + "/" + id + ".buf";
    size_t size = size_t(width) * size_t(height) * 4 * 2;  // two buffers
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (ftruncate(fd, off_t(size)) != 0) {
        ::close(fd);
        return false;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return false;
    plugin_ = &plugin;
    id_ = id;
    for_ = for_module;
    w_ = width;
    h_ = height;
    back_ = 0;
    waiting_ = false;
    map_ = map;
    size_ = size;
    plugin.surfaces_[id] = this;
    Json msg = Json::object();
    msg["t"] = "surface";
    msg["id"] = id;
    msg["w"] = width;
    msg["h"] = height;
    msg["stride"] = width * 4;
    msg["buffers"] = 2;
    if (!for_module.empty()) msg["for"] = for_module;
    plugin.send(msg);
    return true;
}

void Surface::destroy() {
    if (!map_) return;
    munmap(map_, size_);
    map_ = nullptr;
    if (plugin_) {
        plugin_->surfaces_.erase(id_);
        Json msg = Json::object();
        msg["t"] = "surface_destroy";
        msg["id"] = id_;
        plugin_->send(msg);
        ::unlink((plugin_->surface_dir_ + "/" + id_ + ".buf").c_str());
    }
}

uint32_t* Surface::pixels() {
    if (!map_) return nullptr;
    return static_cast<uint32_t*>(map_) + size_t(back_) * size_t(w_) * size_t(h_);
}

void Surface::present() {
    if (!map_ || waiting_) return;
    Json msg = Json::object();
    msg["t"] = "surface_frame";
    msg["id"] = id_;
    msg["buffer"] = back_;
    plugin_->send(msg);
    waiting_ = true;  // until the core has taken it
    back_ ^= 1;
}

void Plugin::text_input(bool active, const std::string& mode) {
    Json msg = Json::object();
    msg["t"] = "text_input";
    msg["active"] = active;
    msg["mode"] = mode;
    send(msg);
}

Canvas& Canvas::poly(std::vector<float> points, std::string color) {
    Json& j = add("poly");
    j["pts"] = points_json(points);
    j["color"] = std::move(color);
    return *this;
}

Canvas& Canvas::polyline(std::vector<float> points, float width, std::string color) {
    Json& j = add("polyline");
    j["pts"] = points_json(points);
    j["width"] = width;
    j["color"] = std::move(color);
    return *this;
}

Canvas& Canvas::text(float x, float y, float w, float h, std::string text, float size, std::string color,
                     std::string align, std::string font) {
    Json& j = add("text");
    j["x"] = x, j["y"] = y, j["w"] = w, j["h"] = h;
    j["text"] = std::move(text);
    j["size"] = size;
    j["color"] = std::move(color);
    j["align"] = std::move(align);
    j["font"] = std::move(font);
    return *this;
}

Canvas& Canvas::icon(float x, float y, float size, std::string name, std::string color) {
    Json& j = add("icon");
    j["x"] = x, j["y"] = y, j["size"] = size;
    j["name"] = std::move(name);
    j["color"] = std::move(color);
    return *this;
}

// ---------------------------------------------------------------- Plugin

Plugin::Plugin(std::string id, std::string version)
    : id_(std::move(id)), version_(std::move(version)) {
    if (const char* d = std::getenv("FACET_PLUGIN_DATA")) data_dir_ = d;
}

void Plugin::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void Plugin::send(const Json& msg) {
    std::string line = msg.dump();
    line += '\n';
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(STDOUT_FILENO, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            running_ = false;  // core is gone
            return;
        }
        off += size_t(n);
    }
}

void Plugin::set_ui(const Screen& screen) {
    if (screen.json() == last_ui_) return;
    last_ui_ = screen.json();
    Json msg = Json::object();
    msg["t"] = "ui";
    msg["root"] = last_ui_;
    send(msg);
}

void Plugin::set_tile(const std::string& subtitle) {
    if (subtitle == last_tile_) return;
    last_tile_ = subtitle;
    Json msg = Json::object();
    msg["t"] = "tile";
    msg["subtitle"] = subtitle;
    send(msg);
}

void Plugin::keyboard_ui(float height, const Canvas& canvas) {
    Json msg = Json::object();
    msg["t"] = "keyboard_ui";
    msg["height"] = height;
    msg["ops"] = canvas.ops();
    send(msg);
}

void Plugin::input(const std::string& action, const std::string& text) {
    Json msg = Json::object();
    msg["t"] = "input";
    msg["action"] = action;
    if (!text.empty()) msg["text"] = text;
    send(msg);
}

bool Plugin::has_permission(const std::string& name) const {
    return std::find(permissions_.begin(), permissions_.end(), name) != permissions_.end();
}

void Plugin::set_badge(int count) { set_badge(count > 0 ? (count > 999 ? "999+" : std::to_string(count)) : ""); }

void Plugin::set_badge(const std::string& text) {
    Json value(text);
    if (value == last_badge_) return;
    last_badge_ = value;
    Json msg = Json::object();
    msg["t"] = "badge";
    msg["value"] = text;
    send(msg);
}

void Plugin::set_tile_icon(const std::string& builtin) {
    Json value(builtin);
    if (value == last_icon_) return;
    last_icon_ = value;
    Json msg = Json::object();
    msg["t"] = "tile_icon";
    msg["name"] = builtin;
    send(msg);
}

void Plugin::set_tile_icon(const Canvas& icon) {
    if (icon.ops() == last_icon_) return;
    last_icon_ = icon.ops();
    Json msg = Json::object();
    msg["t"] = "tile_icon";
    msg["ops"] = icon.ops();
    send(msg);
}

void Plugin::request_permission(const std::string& name, const std::string& reason) {
    Json msg = Json::object();
    msg["t"] = "permission_request";
    msg["name"] = name;
    if (!reason.empty()) msg["reason"] = reason;
    send(msg);
}

void Plugin::release_permission(const std::string& name) {
    permissions_.erase(std::remove(permissions_.begin(), permissions_.end(), name), permissions_.end());
    Json msg = Json::object();
    msg["t"] = "permission_release";
    msg["name"] = name;
    send(msg);
}

Json Notification::to_json() const {
    Json j = Json::object();
    j["id"] = id;
    j["title"] = title;
    j["body"] = body;
    j["kind"] = kind;
    j["priority"] = priority;
    if (!icon.empty()) j["icon"] = icon;
    Json acts = Json::array();
    for (const auto& [aid, label] : actions) {
        Json a = Json::object();
        a["id"] = aid;
        a["label"] = label;
        acts.push_back(a);
    }
    j["actions"] = acts;
    if (timeout_s > 0) j["timeout"] = timeout_s;
    if (!source.empty()) j["source"] = source;
    if (!app_name.empty()) j["app_name"] = app_name;
    return j;
}

void Plugin::notify(const Notification& n) {
    Json msg = Json::object();
    msg["t"] = "notify";
    msg["notification"] = n.to_json();
    send(msg);
}

void Plugin::cancel_notification(const std::string& id, const std::string& source) {
    Json msg = Json::object();
    msg["t"] = "notify_cancel";
    msg["id"] = id;
    if (!source.empty()) msg["source"] = source;
    send(msg);
}

void Plugin::subscribe_notifications(bool on) {
    Json msg = Json::object();
    msg["t"] = "notifications_subscribe";
    msg["value"] = on;
    send(msg);
}

void Plugin::begin_background(const std::string& reason) {
    Json msg = Json::object();
    msg["t"] = "background";
    msg["value"] = true;
    msg["reason"] = reason;
    send(msg);
}

void Plugin::end_background() {
    Json msg = Json::object();
    msg["t"] = "background";
    msg["value"] = false;
    send(msg);
}

void Plugin::wake_lock(bool on) {
    Json msg = Json::object();
    msg["t"] = "wake_lock";
    msg["value"] = on;
    send(msg);
}

void Plugin::report_wayland_clients(const std::vector<WaylandClient>& clients) {
    Json list = Json::array();
    for (const auto& c : clients) {
        Json j = Json::object();
        j["module"] = c.module;
        j["app_id"] = c.app_id;
        j["title"] = c.title;
        j["pid"] = c.pid;
        j["focused"] = c.focused;
        list.push_back(j);
    }
    Json msg = Json::object();
    msg["t"] = "wayland_clients";
    msg["clients"] = list;
    send(msg);
}

std::string Plugin::endpoint(const std::string& capability) const {
    return endpoints_["requires"][capability].str();
}

std::string Plugin::provided_endpoint(const std::string& capability) const {
    return endpoints_["provides"][capability].str();
}

void Plugin::watch_fd(int fd, std::function<void()> on_readable) {
    if (fd >= 0) watched_[fd] = std::move(on_readable);
}

void Plugin::unwatch_fd(int fd) { watched_.erase(fd); }

void Plugin::request_display(bool on) {
    if (last_display_ == int(on)) return;
    last_display_ = int(on);
    Json msg = Json::object();
    msg["t"] = "display";
    msg["on"] = on;
    send(msg);
}

void Plugin::handle(const Json& msg) {
    const std::string& t = msg["t"].str();
    if (t == "hello") {
        if (msg["data_dir"].is_string()) data_dir_ = msg["data_dir"].str();
        permissions_.clear();
        for (const auto& p : msg["permissions"].items()) permissions_.push_back(p.str());
        surface_dir_ = msg["surface_dir"].str();
        screen_w_ = msg["screen"]["w"].as_int();
        screen_h_ = msg["screen"]["h"].as_int();
        endpoints_ = msg["endpoints"];
        lent_.clear();
        catalog_.set_language(i18n::normalize(msg["locale"].str()));
        if (msg["timezone"].is_string()) apply_timezone(msg["timezone"].str());
        content_width_ = msg["content_width"].as_int(content_width_);
        if (msg["screen"].is_object()) {
            screen_w_ = msg["screen"]["w"].as_int(screen_w_);
            screen_h_ = msg["screen"]["h"].as_int(screen_h_);
        }
        Json reply = Json::object();
        reply["t"] = "hello";
        reply["api"] = kApiVersion;
        reply["sdk"] = sdk_version();
        reply["id"] = id_;
        reply["version"] = version_;
        send(reply);
        // Re-send cached state: the core may be a fresh instance.
        if (!last_ui_.is_null()) {
            Json ui = Json::object();
            ui["t"] = "ui";
            ui["root"] = last_ui_;
            send(ui);
        }
        if (on_hello) on_hello(msg);
    } else if (t == "permission") {
        const std::string& name = msg["name"].str();
        bool granted = msg["granted"].as_bool();
        permissions_.erase(std::remove(permissions_.begin(), permissions_.end(), name), permissions_.end());
        if (granted) permissions_.push_back(name);
        if (on_permission) on_permission(name, granted);
    } else if (t == "surface_shown") {
        auto it = surfaces_.find(msg["id"].str());
        if (it != surfaces_.end()) it->second->waiting_ = false;
    } else if (t == "touch") {
        if (on_touch)
            on_touch(msg["surface"].str(), msg["kind"].str(), float(msg["x"].as_number()), float(msg["y"].as_number()));
    } else if (t == "text") {
        if (on_text) on_text(msg["action"].str(), msg["text"].str());
    } else if (t == "insets") {
        if (on_insets) on_insets(std::max(0, msg["bottom"].as_int()));
    } else if (t == "consumer") {
        Consumer c{msg["capability"].str(), msg["module"].str(), msg["dir"].str(), msg["running"].as_bool(),
                   msg["visible"].as_bool()};
        if (on_consumer) on_consumer(c);
    } else if (t == "surface_lent") {
        const std::string& id = msg["id"].str();
        LentSurface s{msg["from"].str(), msg["w"].as_int(), msg["h"].as_int()};
        bool available = msg["available"].as_bool();
        if (available) lent_[id] = s;
        else lent_.erase(id);
        if (on_surface_lent) on_surface_lent(id, s, available);
    } else if (t == "notification_action") {
        if (on_notification_action) on_notification_action(msg["id"].str(), msg["action"].str());
    } else if (t == "notification_posted") {
        if (on_notification_posted) on_notification_posted(msg["notification"]);
    } else if (t == "wayland_client") {
        std::vector<std::string> scopes;
        for (const auto& s : msg["scopes"].items()) scopes.push_back(s.str());
        if (on_wayland_client) on_wayland_client(msg["module"].str(), scopes, msg["running"].as_bool());
    } else if (t == "ping") {
        Json reply = Json::object();
        reply["t"] = "pong";
        reply["seq"] = msg["seq"];
        send(reply);
    } else if (t == "visible") {
        visible_ = msg["value"].as_bool();
        if (on_visible) on_visible(visible_);
    } else if (t == "event") {
        if (msg["action"].str() == "submit") {
            if (on_submit) on_submit(msg["id"].str(), msg["value"].str());
        } else if (on_event) {
            on_event(msg["id"].str(), msg["value"]);
        }
    } else if (t == "keyboard_show") {
        std::vector<std::string> langs;
        for (const auto& l : msg["langs"].items()) langs.push_back(l.str());
        if (on_keyboard_show) on_keyboard_show(msg["mode"].as_string("text"), float(msg["width"].as_number(400)), langs);
    } else if (t == "keyboard_key") {
        if (on_keyboard_key) on_keyboard_key(msg["hit"].str());
    } else if (t == "keyboard_hide") {
        if (on_keyboard_hide) on_keyboard_hide();
    } else if (t == "layout") {
        if (msg["screen"].is_object()) {
            screen_w_ = msg["screen"]["w"].as_int(screen_w_);
            screen_h_ = msg["screen"]["h"].as_int(screen_h_);
        }
        int w = msg["content_width"].as_int(content_width_);
        if (w != content_width_) {
            content_width_ = w;
            last_ui_ = Json();  // force re-sending the tree built for the new width
            if (on_layout) on_layout(w);
        }
    } else if (t == "timezone") {
        apply_timezone(msg["value"].str());
    } else if (t == "locale") {
        set_locale(i18n::normalize(msg["value"].str()));
    } else if (t == "activity") {
        if (on_activity) on_activity();
    } else if (t == "shutdown") {
        running_ = false;
    }
}

void Plugin::set_locale(const std::string& lang) {
    catalog_.set_language(lang);
    last_tile_.clear();  // force re-sending translated state
    last_ui_ = Json();
    if (on_locale) on_locale(catalog_.language());
}

int Plugin::run(int tick_ms) {
    signal(SIGPIPE, SIG_IGN);
    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now();
    std::string buf;
    char chunk[4096];

    while (running_) {
        auto now = clock::now();
        int timeout = int(std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count());
        if (timeout < 0) timeout = 0;

        std::vector<pollfd> fds{{STDIN_FILENO, POLLIN, 0}};
        for (const auto& [fd, cb] : watched_) fds.push_back({fd, POLLIN, 0});
        int r = ::poll(fds.data(), nfds_t(fds.size()), timeout);
        if (r < 0 && errno != EINTR) break;

        for (size_t i = 1; r > 0 && i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            auto it = watched_.find(fds[i].fd);  // a callback may have removed it
            if (it == watched_.end()) continue;
            auto cb = it->second;
            cb();
        }

        if (r > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
            if (n == 0) break;  // core closed the pipe
            if (n < 0 && errno != EINTR && errno != EAGAIN) break;
            if (n > 0) buf.append(chunk, size_t(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                Json msg;
                if (Json::parse(line, msg)) handle(msg);
                if (!running_) break;
            }
        }

        if (clock::now() >= next_tick) {
            if (on_tick) on_tick();
            next_tick = clock::now() + std::chrono::milliseconds(tick_ms);
        }
    }
    if (on_shutdown) on_shutdown();
    return 0;
}

}  // namespace facet::sdk
