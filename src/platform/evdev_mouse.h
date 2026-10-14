// A mouse (or an air mouse / remote with a pointer) through /dev/input/event*:
// relative motion, the left button and the wheel become the same events as a
// finger on the screen. Off until switched on (Settings > Input); the devices
// are only scanned for while it is off, so the setting can say whether there
// is one.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "platform/platform.h"

namespace facet::platform {

class EvdevMouse {
public:
    ~EvdevMouse();
    void set_enabled(bool on);
    bool enabled() const { return enabled_; }
    // A mouse is plugged in (found by the last scan).
    bool available() const { return available_; }
    void add_poll_fds(std::vector<pollfd>& fds) const;
    // Events appended to `out` carry `mouse = true`. Rescans every few
    // seconds (hotplug). The pointer lives on a screen of w x h.
    void pump(std::vector<Event>& out, double now, int w, int h);
    float x() const { return x_; }
    float y() const { return y_; }

private:
    struct Device {
        int fd = -1;
        std::string path;
    };
    // Finds mice: with REL_X, REL_Y and a left button.
    std::vector<std::string> scan() const;
    void open_all();
    void close_all();
    void read_device(Device& d, std::vector<Event>& out, int w, int h);

    std::vector<Device> devices_;
    bool enabled_ = false, available_ = false;
    double last_scan_ = -100;
    float x_ = -1, y_ = -1;
    bool down_ = false;
    std::set<std::string> announced_;
};

}  // namespace facet::platform
