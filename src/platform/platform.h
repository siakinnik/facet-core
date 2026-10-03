// Output/input/power backend. Implementations: fbdev (device), gpu (the
// GPU helper with DRM/KMS and OpenGL ES, optional), x11 and headless (dev).
#pragma once

#include <poll.h>

#include <memory>
#include <string>
#include <vector>

#include "gfx/canvas.h"

namespace facet::platform {

enum class EventType { Down, Up, Move, Resize, Quit };

struct Event {
    EventType type;
    float x = 0, y = 0;
    int w = 0, h = 0;
};

// A plugin surface the GPU composes under the canvas (where the canvas is
// see-through), instead of the core copying its pixels.
struct Layer {
    std::string key;   // owner/surface#generation: a new key for a new buffer file
    std::string path;  // the surface's buffer file
    int w = 0, h = 0, stride = 0, buffers = 0, buffer = 0;
    gfx::Rect dst, clip;
    // A GPU buffer (dma-buf) shown instead of the shared memory, slot -1 = none.
    int gpu = -1, gpu_fd = -1, gpu_w = 0, gpu_h = 0;
    uint32_t gpu_format = 0, gpu_offset = 0, gpu_stride = 0, gpu_generation = 0;
    uint64_t gpu_modifier = 0;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual const char* name() const = 0;
    virtual bool init() = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    // Registers fds to wake the main loop, then drains pending events.
    virtual void add_poll_fds(std::vector<pollfd>& fds) = 0;
    virtual void pump(std::vector<Event>& out) = 0;

    virtual void present(const gfx::Canvas& canvas) = 0;

    // ---- GPU composition (only backends that support layers).
    virtual bool supports_layers() const { return false; }
    // Memory the canvas should draw into (shared with the GPU), or null.
    virtual uint32_t* canvas_memory() { return nullptr; }
    // False while the GPU still reads the canvas of the previous frame.
    virtual bool ready_to_draw() const { return true; }
    // Waits up to `ms` for ready_to_draw() (a frame that cannot be put off).
    virtual void wait_ready(int ms) { (void)ms; }
    // Shows the canvas (if it changed) over the layers.
    virtual void present_layers(const gfx::Canvas& canvas, bool canvas_changed, const std::vector<Layer>& layers) {
        (void)layers;
        if (canvas_changed) present(canvas);
    }
    // Keys of layers whose buffers the GPU has taken since the last call.
    virtual std::vector<std::string> take_uploaded() { return {}; }

    // ---- GPU drawing: the helper draws the interface from recorded
    // commands (gfx/ops.h) instead of the core's pixels.
    virtual bool supports_ops() const { return false; }
    // Layers may be GPU buffers (Layer::gpu): shown without a copy.
    virtual bool supports_gpu_buffers() const { return false; }
    // `ops` empty: the previous frame's commands again (only surfaces changed).
    virtual void present_ops(const std::vector<uint8_t>& ops, const std::vector<Layer>& layers) {
        (void)ops;
        (void)layers;
    }
    // True once after the helper (re)started: it has no uploaded masks.
    virtual bool take_ops_lost() { return false; }
    virtual void set_display_power(bool on) = 0;
    virtual bool has_brightness() const { return false; }
    virtual void set_brightness(int percent) { (void)percent; }
};

struct Options {
    bool gpu = false;      // use the GPU helper (Settings > Graphics)
    std::string card;      // DRM device, e.g. /dev/dri/card0
    std::string gl_dir;    // the installed OpenGL package
    std::string data_dir;  // for the shader cache
};

// FACET_BACKEND=fbdev|gpu|x11|headless selects explicitly; otherwise x11 when
// DISPLAY is set (and compiled in), else gpu when enabled and working, else
// fbdev.
std::unique_ptr<Platform> create_platform(const Options& options);
// Why the GPU backend was not used this time ("" if it is used or off).
const std::string& gpu_failure();

}  // namespace facet::platform
