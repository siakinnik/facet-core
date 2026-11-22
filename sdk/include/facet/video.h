// Video in a module's own picture, ready to use: the SDK runs mpv, takes its
// frames and shows them in a Surface; the module puts that surface on its
// screen (Screen::surface(), or anywhere on a free-form screen) and may draw
// its own controls over it.
//
// mpv stays a separate program (the module brings it, e.g. downloaded with
// its libraries; see facet-tv), so the module's binary links nothing new.
// mpv draws into shared memory for a tiny display server inside the SDK
// (--vo=wlshm): no Wayland module is needed. Sound goes out through ALSA
// (permission "audio"). mpv is controlled over its JSON IPC socket.
//
//     facet::sdk::Video video(plugin, "tv");
//     video.set_player({"/data/runtime/bin/mpv"});
//     video.on_state = [&](const facet::sdk::Video::State& s) { ... };
//     video.play("https://example.org/stream.m3u8", 1280, 720);
//     screen.surface("tv");                 // or FreeScreen::surface(...)
#pragma once

#include <sys/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "facet/json.h"
#include "facet/plugin.h"

namespace facet::sdk {

namespace detail {
class WlServer;
}

class Video {
public:
    struct State {
        bool running = false;    // the player process runs
        bool playing = false;    // frames are coming (not paused, not buffering, not ended)
        bool paused = false;
        bool buffering = false;  // waiting for the network
        bool ended = false;      // the file or stream ended
        double position = -1;    // seconds, -1 if unknown
        double duration = -1;    // seconds, -1 for live streams / unknown
        int volume = 100;
        std::string error;       // why playback stopped, if it failed
    };

    Video(Plugin& plugin, std::string surface_id);
    ~Video();
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    // How to run mpv: its command up to the options (default {"mpv"}), e.g.
    // {"/data/rt/lib/ld-linux.so", "--library-path", "/data/rt/lib", "/data/rt/bin/mpv"},
    // and extra environment ("NAME=value").
    void set_player(std::vector<std::string> command, std::vector<std::string> env = {});

    // Starts playing `url` (anything mpv opens) into a `width` x `height`
    // surface; the picture keeps its aspect ratio inside it. `options` are
    // more mpv options ("--hwdec=auto-copy", "--cache=yes", ...). A running
    // player is replaced.
    bool play(const std::string& url, int width, int height, const std::vector<std::string>& options = {});
    void stop();
    bool running() const { return pid_ > 0; }

    void pause(bool on);
    void set_volume(int percent);  // 0..100 (mpv's own volume; the system's is Facet's)
    void seek(double seconds, bool relative = false);
    // A surface of another size (e.g. the screen turned): mpv draws for it from the next frame.
    void resize(int width, int height);
    // Any mpv command, e.g. Json::Array{"set_property", "speed", 1.5}.
    void command(const Json& args);

    const State& state() const { return state_; }
    Surface& surface() { return surface_; }

    // Called when the state changes (the position at most four times a second).
    std::function<void(const State& state)> on_state;
    // The player process ended (exit status), whatever the reason.
    std::function<void(int status)> on_exit;
    // Every line mpv prints (warnings and errors by default); logged with Plugin::log if unset.
    std::function<void(const std::string& line)> on_log;

private:
    void start_server(int w, int h);
    void on_frame(const uint32_t* px, int w, int h, int stride);
    void flush_frame();
    void read_output();
    void connect_ipc();
    void read_ipc();
    void send_ipc(const Json& msg);
    void handle_ipc(const Json& msg);
    void reap(bool wait);
    void notify(bool position_only);
    void kill_player();

    Plugin& plugin_;
    std::string id_;
    Surface surface_;
    std::unique_ptr<detail::WlServer> server_;
    std::vector<std::string> command_{"mpv"}, env_;
    std::string dir_;           // the display and IPC sockets
    pid_t pid_ = -1;
    int out_fd_ = -1;           // mpv's stdout + stderr
    int ipc_fd_ = -1;
    int timer_fd_ = -1;         // retries the IPC connection while mpv starts
    std::string out_buf_, ipc_buf_;
    std::vector<std::string> ipc_queue_;  // commands sent before the socket was there
    std::vector<uint32_t> pending_;       // the newest frame while the core still shows the last one
    int pending_w_ = 0, pending_h_ = 0;
    bool has_pending_ = false;
    State state_;
    double notified_at_ = 0;
    int width_ = 0, height_ = 0;
};

}  // namespace facet::sdk
