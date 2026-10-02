// Example plugin: the API 3 features in one screen. Notifications and an
// incoming call, a transient permission (the camera "while in use"), a badge
// on the tile, a wake lock, background work, and a Surface (own pixels with
// touches and typed text, in the page or full screen). Also a template for
// plugins that use them.
#include <dirent.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include <string>

#include "facet/plugin.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::Notification;
using facet::sdk::Plugin;
using facet::sdk::Screen;
using facet::sdk::Surface;

namespace {

// Camera devices visible inside our container right now.
int count_cameras() {
    int n = 0;
    if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d))
            if (std::strncmp(e->d_name, "video", 5) == 0) ++n;
        closedir(d);
    }
    return n;
}

class Showcase {
public:
    explicit Showcase(Plugin& plugin) : plugin_(plugin) {}

    // A finger-painting surface: a gradient, dots where it was touched.
    void ensure_surface() {
        if (!plugin_.has_permission("display.surface")) return;
        int w = fullscreen_ && plugin_.screen_width() > 0 ? plugin_.screen_width() : 640;
        int h = fullscreen_ && plugin_.screen_height() > 0 ? plugin_.screen_height() : 360;
        if (surface_.valid() && surface_.width() == w && surface_.height() == h) return;
        if (!surface_.create(plugin_, "paint", w, h)) return;
        dots_.clear();
        redraw_ = 2;  // both buffers
    }

    void tick() {
        if (!surface_.valid() || redraw_ == 0 || !surface_.ready()) return;
        uint32_t* px = surface_.pixels();
        int w = surface_.width(), h = surface_.height();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[y * w + x] = (uint32_t(40 + 60 * x / w) << 16) | (uint32_t(40 + 80 * y / h) << 8) | 120u;
        for (const auto& [dx, dy] : dots_) {
            int r = std::max(4, w / 80);
            for (int y = std::max(0, int(dy) - r); y < std::min(h, int(dy) + r); ++y)
                for (int x = std::max(0, int(dx) - r); x < std::min(w, int(dx) + r); ++x)
                    if ((x - dx) * (x - dx) + (y - dy) * (y - dy) <= float(r * r)) px[y * w + x] = 0xFFFFFF;
        }
        surface_.present();
        --redraw_;
    }

    void on_touch(const std::string&, const std::string& kind, float x, float y) {
        if (kind != "up" && dots_.size() < 4000) dots_.emplace_back(x, y);
        redraw_ = 2;
        touches_++;
        refresh();
    }

    void on_text(const std::string& action, const std::string& text) {
        if (action == "insert") typed_ += text;
        else if (action == "backspace" && !typed_.empty()) typed_.pop_back();
        else if (action == "enter" || action == "hide") plugin_.text_input(false);
        refresh();
    }

    void on_event(const std::string& id, const Json& v) {
        if (id == "fullscreen") {
            fullscreen_ = true;
            ensure_surface();
        } else if (id == "type") {
            plugin_.text_input(true);
        } else if (id == "notify") {
            Notification n;
            n.id = "hello-" + std::to_string(++sent_);
            n.title = tr("Hello from the showcase");
            n.body = tr("Notification number {}", {std::to_string(sent_)});
            n.actions = {{"reply", tr("Reply")}};
            plugin_.notify(n);
            plugin_.set_badge(sent_);
        } else if (id == "call") {
            Notification n;
            n.id = "call";
            n.kind = "call";
            n.title = "Anna Petrova";
            n.body = tr("Video call");
            n.timeout_s = 30;
            plugin_.notify(n);
        } else if (id == "camera") {
            plugin_.request_permission("camera", tr("To show that transient access works."));
            status_ = tr("asking for the camera…");
        } else if (id == "release") {
            plugin_.release_permission("camera");
            status_ = tr("camera released");
        } else if (id == "wake") {
            wake_ = v.as_bool();
            plugin_.wake_lock(wake_);
        } else if (id == "work") {
            working_ = v.as_bool();
            if (working_) plugin_.begin_background(tr("Pretending to sync"));
            else plugin_.end_background();
        } else if (id == "clear") {
            sent_ = 0;
            plugin_.set_badge(0);
        }
        refresh();
    }

    void on_permission(const std::string& name, bool granted) {
        status_ = name + ": " + (granted ? tr("granted") : tr("not granted"));
        refresh();
    }

    void on_action(const std::string& id, const std::string& action) {
        status_ = tr("notification {}: {}", {id, action});
        if (id == "call" && action == "accept") in_call_ = true;
        refresh();
    }

    void refresh() {
        if (!plugin_.visible() && fullscreen_) {  // the user swiped back
            fullscreen_ = false;
            plugin_.text_input(false);
        }
        ensure_surface();
        plugin_.set_tile(sent_ ? tr("Sent: {}", {std::to_string(sent_)}) : tr("Nothing sent yet"));
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    std::string tr(std::string_view k) const { return plugin_.tr(k); }
    std::string tr(std::string_view k, const std::vector<std::string>& a) const { return plugin_.tr(k, a); }

    Screen build() const {
        Screen ui(tr("SDK showcase"));
        if (fullscreen_ && surface_.valid()) ui.fullscreen("paint");

        ui.section(tr("Own picture"));
        if (!plugin_.has_permission("display.surface")) {
            ui.note(tr("The “Own picture” permission is off, so there is nothing to draw on."));
        } else {
            ui.surface("paint");
            ui.info(tr("Touches"), std::to_string(touches_));
            ui.button("fullscreen", tr("Full screen (swipe down from the top to leave)"));
            ui.button("type", tr("Type with Facet's keyboard"));
            if (!typed_.empty()) ui.info(tr("Typed"), typed_);
        }

        ui.section(tr("Notifications"));
        if (!plugin_.has_permission("notifications"))
            ui.note(tr("The notifications permission is off: nothing will appear. The plugin keeps working."));
        ui.button("notify", tr("Send a notification"), "primary");
        ui.button("call", tr("Simulate an incoming call"));
        ui.button("clear", tr("Clear the badge"));
        if (in_call_) ui.info(tr("Call"), tr("accepted"), "good");

        ui.section(tr("Camera while in use"));
        ui.info(tr("Camera devices I can see"), std::to_string(count_cameras()));
        ui.button("camera", tr("Use the camera"));
        ui.button("release", tr("Release the camera"));

        ui.section(tr("System"));
        ui.toggle("wake", tr("Keep the screen on"), wake_);
        ui.toggle("work", tr("Background work"), working_);
        std::string perms;
        for (const auto& p : plugin_.permissions()) perms += (perms.empty() ? "" : ", ") + p;
        ui.info(tr("Permissions now"), perms.empty() ? "—" : perms);
        if (!status_.empty()) ui.info(tr("Last event"), status_);
        return ui;
    }

    Plugin& plugin_;
    Surface surface_;
    std::vector<std::pair<float, float>> dots_;
    int redraw_ = 0, touches_ = 0;
    bool fullscreen_ = false;
    std::string typed_;
    int sent_ = 0;
    bool wake_ = false, working_ = false, in_call_ = false;
    std::string status_;
};

}  // namespace

int main() {
    Plugin plugin("example-showcase", "0.0.2");  // keep in sync with manifest.json
    showcase::register_translations(plugin.catalog());
    Showcase app(plugin);
    plugin.on_hello = [&](const Json&) { app.refresh(); };
    plugin.on_event = [&](const std::string& id, const Json& v) { app.on_event(id, v); };
    plugin.on_permission = [&](const std::string& name, bool granted) { app.on_permission(name, granted); };
    plugin.on_notification_action = [&](const std::string& id, const std::string& a) { app.on_action(id, a); };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_touch = [&](const std::string& s, const std::string& k, float x, float y) { app.on_touch(s, k, x, y); };
    plugin.on_text = [&](const std::string& a, const std::string& t) { app.on_text(a, t); };
    plugin.on_tick = [&] { app.tick(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    return plugin.run(30);
}
