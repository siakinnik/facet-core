// /sys/class/backlight control (brightness and panel power), or the
// lcd-backlight LED of MediaTek and other Android kernels.
#pragma once

#include <string>

namespace facet::platform {

class Backlight {
public:
    bool open();  // picks the first backlight device
    bool available() const { return !dir_.empty(); }
    void set_percent(int percent);
    void set_power(bool on);

private:
    bool open_led();

    std::string dir_;
    bool led_ = false;  // /sys/class/leds/lcd-backlight
    int max_ = 0;
    int percent_ = 100;
};

}  // namespace facet::platform
