#include "facet/plugin.h"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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

Screen& Screen::canvas(std::string id, float height, const Canvas& canvas) {
    Json& j = add("canvas");
    j["id"] = std::move(id);
    j["height"] = height;
    j["ops"] = canvas.ops();
    return *this;
}

// ---------------------------------------------------------------- Canvas

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
        catalog_.set_language(i18n::normalize(msg["locale"].str()));
        if (msg["timezone"].is_string()) apply_timezone(msg["timezone"].str());
        content_width_ = msg["content_width"].as_int(content_width_);
        Json reply = Json::object();
        reply["t"] = "hello";
        reply["api"] = kApiVersion;
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
    } else if (t == "ping") {
        Json reply = Json::object();
        reply["t"] = "pong";
        reply["seq"] = msg["seq"];
        send(reply);
    } else if (t == "visible") {
        visible_ = msg["value"].as_bool();
        if (on_visible) on_visible(visible_);
    } else if (t == "event") {
        if (on_event) on_event(msg["id"].str(), msg["value"]);
    } else if (t == "layout") {
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

        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeout);
        if (r < 0 && errno != EINTR) break;

        if (r > 0) {
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
