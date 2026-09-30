// Touchscreen input via /dev/input/event* (single-touch and multitouch
// protocol B; only the first contact is tracked).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "platform/platform.h"

namespace facet::platform {

class EvdevTouch {
public:
    ~EvdevTouch();
    void open_devices(int screen_w, int screen_h);
    void set_screen_size(int w, int h) { sw_ = w, sh_ = h; }
    void add_poll_fds(std::vector<pollfd>& fds);
    void pump(std::vector<Event>& out, double now);

private:
    struct Device {
        int fd = -1;
        int min_x = 0, max_x = 1, min_y = 0, max_y = 1;
        bool mt = false;
        bool direct = false;  // INPUT_PROP_DIRECT: touchscreen, not touchpad
        int slot = 0;
        int raw_x = 0, raw_y = 0;
        bool down = false, was_down = false, moved = false;
    };
    void read_device(Device& d, std::vector<Event>& out);
    Event map(const Device& d, EventType type) const;

    std::vector<Device> devices_;
    int sw_ = 0, sh_ = 0;
    // FACET_TOUCH_TRANSFORM: comma list of swap, invx, invy.
    bool swap_ = false, inv_x_ = false, inv_y_ = false;
    double last_scan_ = 0;
    std::set<std::string> announced_;  // log each device once, not on every rescan
};

}  // namespace facet::platform
