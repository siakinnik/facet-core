// /sys/class/backlight control (brightness and panel power).
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
    std::string dir_;
    int max_ = 0;
    int percent_ = 100;
};

}  // namespace facet::platform
