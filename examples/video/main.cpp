// Example plugin: video on a free-form screen, with the module's own
// controls drawn over the picture. A template for modules that play video.
//
// facet::sdk::Video runs mpv and shows its frames in a surface; FreeScreen
// places that surface and the module's drawing anywhere on the screen. The
// module brings mpv itself (here: the system's "mpv", or the command in
// <data>/player.json: {"command": ["/path/to/mpv"], "url": "..."}).
// By default it plays mpv's own test picture, so it needs no network.
#include <cstdio>
#include <string>

#include "facet/plugin.h"
#include "facet/video.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::Canvas;
using facet::sdk::FreeScreen;
using facet::sdk::Num;
using facet::sdk::Plugin;
using facet::sdk::Video;

namespace {

class VideoDemo {
public:
    explicit VideoDemo(Plugin& plugin) : plugin_(plugin), video_(plugin, "video") {
        video_.on_state = [this](const Video::State&) { refresh(); };
        video_.on_exit = [this](int status) {
            Plugin::log("player ended (status %d)", status);
            refresh();
        };
    }

    void start() {
        Json cfg;
        if (facet::load_json_file(plugin_.data_dir() + "/player.json", cfg)) {
            std::vector<std::string> cmd;
            for (const auto& a : cfg["command"].items()) cmd.push_back(a.str());
            if (!cmd.empty()) video_.set_player(cmd);
            url_ = cfg["url"].as_string(url_);
        }
        refresh();
    }

    void on_visible(bool visible) {
        if (visible && !video_.running()) video_.play(url_, 1280, 720);
        if (!visible) video_.stop();  // nobody watches: no decoding
        refresh();
    }

    void on_event(const std::string& id, const Json& v) {
        if (id != "screen") return;
        const std::string& hit = v.str();
        if (hit == "pause") video_.pause(!video_.state().paused);
        else if (hit == "louder") video_.set_volume(video_.state().volume + 10);
        else if (hit == "quieter") video_.set_volume(video_.state().volume - 10);
        else if (hit == "toggle") controls_ = !controls_;
        refresh();
    }

    void refresh() {
        const Video::State& st = video_.state();
        FreeScreen s("#000000");
        s.surface("video", 0, 0, Num::of(1), Num::of(0, 1), "contain");
        Canvas c;
        if (controls_) {
            // A bar at the top: title, state, logo.
            c.rect(0, 0, Num::of(1), 64, "#00000099");
            c.image(16, 12, 40, 40, "plugin/logo.png", "contain", 10);
            c.text(68, 8, Num::of(1, 0, -200), 28, plugin_.tr("Video demo"), 18, "#FFFFFF", "start", "medium");
            std::string state = !st.error.empty() ? st.error
                                : !st.running     ? plugin_.tr("stopped")
                                : st.buffering    ? plugin_.tr("loading…")
                                : st.paused       ? plugin_.tr("paused")
                                                  : plugin_.tr("playing");
            if (st.position >= 0) {
                char t[32];
                std::snprintf(t, sizeof t, " · %d:%02d", int(st.position) / 60, int(st.position) % 60);
                state += t;
            }
            c.text(68, 34, Num::of(1, 0, -200), 22, state, 13, "#FFFFFFB0");
            // Buttons at the bottom right: quieter, pause, louder.
            c.circle(Num::of(1, 0, -200), Num::of(0, 1, -50), 26, "#00000099", "quieter", "#FFFFFF40");
            c.text(Num::of(1, 0, -226), Num::of(0, 1, -76), 52, 52, "−", 28, "#FFFFFF", "center");
            c.circle(Num::of(1, 0, -130), Num::of(0, 1, -50), 30, "accent", "pause");
            if (st.paused) {  // play: a triangle (drawn, not a font glyph: fonts may lack it)
                float cx = plugin_.screen_dp_width() - 130, cy = plugin_.screen_dp_height() - 50;
                c.poly({cx - 8, cy - 12, cx + 13, cy, cx - 8, cy + 12}, "on_accent");
            } else {  // pause: two bars
                c.rrect(Num::of(1, 0, -141), Num::of(0, 1, -62), 8, 24, 2, "on_accent");
                c.rrect(Num::of(1, 0, -127), Num::of(0, 1, -62), 8, 24, 2, "on_accent");
            }
            c.circle(Num::of(1, 0, -60), Num::of(0, 1, -50), 26, "#00000099", "louder", "#FFFFFF40");
            c.text(Num::of(1, 0, -86), Num::of(0, 1, -76), 52, 52, "+", 28, "#FFFFFF", "center");
            c.text(16, Num::of(0, 1, -40), 200, 24, plugin_.tr("Volume {}%", {std::to_string(st.volume)}), 13,
                   "#FFFFFFB0");
        }
        // Last, so the buttons above win: a tap anywhere else shows or hides the controls.
        c.rect(0, 64, Num::of(1), Num::of(0, 1, -150), "#00000000", "toggle");
        s.draw(c);
        plugin_.set_ui(s);
    }

private:
    Plugin& plugin_;
    Video video_;
    std::string url_ = "av://lavfi:testsrc2=size=1280x720:rate=25";
    bool controls_ = true;
};

}  // namespace

int main() {
    Plugin plugin("example-video", "0.1.0");  // keep in sync with manifest.json
    example_video::register_translations(plugin.catalog());
    VideoDemo app(plugin);
    plugin.on_hello = [&](const Json&) { app.start(); };
    plugin.on_visible = [&](bool visible) { app.on_visible(visible); };
    plugin.on_event = [&](const std::string& id, const Json& v) { app.on_event(id, v); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_layout = [&](int) { app.refresh(); };
    return plugin.run();
}
