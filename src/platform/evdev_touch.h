// Touchscreen input via /dev/input/event* (single-touch and multitouch
// protocols A and B; only the first contact is tracked). Android's virtual
// keys (/sys/board_properties/virtualkeys.<device name>: Back, Home and Menu
// drawn under the screen, inside the touch area) become key events.
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
    // Multitouch protocol, told by the events themselves: drivers declare
    // ABS_MT_TRACKING_ID in their capabilities without ever sending it.
    enum class Proto { Unknown, A, B };
    // A virtual key's hit box, in screen pixels (may lie past the screen's edge).
    struct VirtualKey {
        Key key = Key::Unknown;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    struct Device {
        int fd = -1;
        std::string name;  // event2, for the log
        int min_x = 0, max_x = 1, min_y = 0, max_y = 1;
        bool mt = false;
        bool direct = false;  // INPUT_PROP_DIRECT: touchscreen, not touchpad
        Proto proto = Proto::Unknown;
        int slot = 0;
        int raw_x = 0, raw_y = 0;
        bool down = false, was_down = false, moved = false;
        // Protocol A: contacts are separated by SYN_MT_REPORT within a frame.
        int contact = 0, contacts = 0;           // index of the contact being read, contacts with a position
        bool contact_pos = false, frame_mt = false;
        bool btn_touch = false, btn_tool = false, btn_known = false, btn_event = false;
        std::vector<VirtualKey> vkeys;
        Key vkey = Key::Unknown;  // the virtual key the finger went down on
    };
    std::vector<VirtualKey> load_virtual_keys(const std::string& device_name) const;
    Key virtual_key_at(const Device& d) const;
    void read_device(Device& d, std::vector<Event>& out);
    void set_proto(Device& d, Proto p) const;
    Event map(const Device& d, EventType type) const;

    bool debug_ = false;  // FACET_DEBUG_INPUT
    std::vector<Device> devices_;
    int sw_ = 0, sh_ = 0;
    // FACET_TOUCH_TRANSFORM: comma list of swap, invx, invy.
    bool swap_ = false, inv_x_ = false, inv_y_ = false;
    double last_scan_ = 0;
    std::set<std::string> announced_;  // log each device once, not on every rescan
};

}  // namespace facet::platform
