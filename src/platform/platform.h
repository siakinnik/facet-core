// Output/input/power backend. Implementations: fbdev (device), x11 (dev).
// A DRM/KMS backend can be added behind the same interface.
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
    virtual void set_display_power(bool on) = 0;
    virtual bool has_brightness() const { return false; }
    virtual void set_brightness(int percent) { (void)percent; }
};

// FACET_BACKEND=fbdev|x11|headless selects explicitly; otherwise x11 when DISPLAY is
// set (and compiled in), else fbdev.
std::unique_ptr<Platform> create_platform();

}  // namespace facet::platform
