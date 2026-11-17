// Hardware buttons through /dev/input/event*: volume, power and the Android
// row under the screen (Back, Home, Menu, recent apps), from gpio-keys, the
// SoC's keypad, the PMIC's power key or the touchscreen driver itself.
// Button-only devices are grabbed, so the system (e.g. logind) does not act
// on the power key as well; FACET_KEYS=off leaves them all alone.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "platform/platform.h"

namespace facet::platform {

class EvdevKeys {
public:
    ~EvdevKeys();
    void open_devices();
    bool available() const { return !devices_.empty(); }
    void add_poll_fds(std::vector<pollfd>& fds) const;
    // Rescans now and then (hotplug, a device that went away).
    void pump(std::vector<Event>& out, double now);

private:
    struct Device {
        int fd = -1;
        std::string path;
        bool keyboard = false;
    };
    void close_all();
    void read_device(Device& d, std::vector<Event>& out);

    std::vector<Device> devices_;
    double last_scan_ = 0;
    bool debug_ = false;  // FACET_DEBUG_INPUT
    std::set<std::string> announced_;
};

}  // namespace facet::platform
