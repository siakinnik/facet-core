// A very small Wayland display server for one client: a video player (mpv
// with --vo=wlshm) drawing into shared memory. It speaks just what such a
// client needs (registry, compositor, surfaces, wl_shm, one output,
// xdg-shell) and hands every committed frame to its owner, so a module can
// show the player's picture in its own Facet surface without the Wayland
// module, and without linking any library: module binaries stay static.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace facet::sdk::detail {

class WlServer {
public:
    ~WlServer() { stop(); }

    // Listens on `dir`/wayland-0 (WAYLAND_DISPLAY for the client, an
    // absolute path; `dir` is also its XDG_RUNTIME_DIR). Windows are asked to
    // be `w` x `h`.
    bool start(const std::string& dir, int w, int h);
    void stop();
    const std::string& socket_path() const { return path_; }
    int listen_fd() const { return listen_; }
    int client_fd() const { return client_; }

    void accept_client();
    // Reads and handles what the client sent; false once it is gone.
    bool read_client();
    // Asks the window for another size.
    void resize(int w, int h);

    // A committed frame: 32-bit pixels, X/ARGB8888 (the same memory layout
    // as a Facet surface), valid during the call only.
    std::function<void(const uint32_t* pixels, int w, int h, int stride_px)> on_frame;
    std::function<void()> on_connect;

private:
    enum class Kind {
        Display, Registry, Callback, Compositor, Surface, Region, Shm, Pool, Buffer, Output, WmBase, Positioner,
        XdgSurface, Toplevel, Unknown
    };
    struct Object {
        Kind kind = Kind::Unknown;
        // Pool
        int fd = -1;
        uint8_t* map = nullptr;
        size_t size = 0;
        // Buffer
        uint32_t pool = 0;
        int32_t offset = 0, w = 0, h = 0, stride = 0;
        uint32_t format = 0;
        // Surface
        uint32_t attached = 0;      // buffer of the next commit
        bool attach_pending = false;
        std::vector<uint32_t> frame_callbacks;
        uint32_t role = 0;          // its xdg_surface
        bool configured = false;
        // XdgSurface: its wl_surface; Toplevel: its xdg_surface
        uint32_t parent = 0;
    };

    class Message;
    void send(const Message& m);
    void handle(uint32_t id, uint16_t opcode, const uint8_t* args, size_t len);
    void destroy(uint32_t id);  // the client destroyed it: forget it and free the id
    void bind(uint32_t name, uint32_t id);
    void commit(uint32_t surface);
    void configure_toplevel(uint32_t toplevel);
    void close_client();
    void unmap_pool(Object& p);

    std::string dir_, path_, lock_path_;
    int listen_ = -1, lock_ = -1, client_ = -1;
    int w_ = 640, h_ = 360;
    uint32_t serial_ = 1;
    std::vector<uint8_t> in_;
    std::deque<int> fds_;  // received with the messages, taken in order
    std::map<uint32_t, Object> objects_;
};

}  // namespace facet::sdk::detail
