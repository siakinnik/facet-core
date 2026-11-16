#include "platform/backlight.h"

#include <dirent.h>

#include <algorithm>
#include <fstream>

namespace facet::platform {

namespace {
bool write_file(const std::string& path, const std::string& value) {
    std::ofstream f(path);
    if (!f) return false;
    f << value;
    return bool(f);
}
}  // namespace

bool Backlight::open() {
    const char* root = "/sys/class/backlight";
    DIR* d = opendir(root);
    if (!d) return open_led();
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string dir = std::string(root) + "/" + e->d_name;
        std::ifstream f(dir + "/max_brightness");
        int m = 0;
        if (f >> m && m > 0) {
            dir_ = dir;
            max_ = m;
            break;
        }
    }
    closedir(d);
    return available() || open_led();
}

// MediaTek and many other Android kernels drive the panel's backlight as an
// LED class device. Only this one name: other LEDs are not the screen.
bool Backlight::open_led() {
    const std::string dir = "/sys/class/leds/lcd-backlight";
    std::ifstream f(dir + "/max_brightness");
    int m = 0;
    if (!(f >> m) || m <= 0) return false;
    dir_ = dir;
    max_ = m;
    led_ = true;
    return true;
}

void Backlight::set_percent(int percent) {
    if (!available()) return;
    percent_ = std::clamp(percent, 1, 100);
    int v = std::max(1, max_ * percent_ / 100);
    write_file(dir_ + "/brightness", std::to_string(v));
}

void Backlight::set_power(bool on) {
    if (!available()) return;
    if (led_) {  // no bl_power: off is brightness 0
        if (on) set_percent(percent_);
        else write_file(dir_ + "/brightness", "0");
        return;
    }
    // FB_BLANK_UNBLANK = 0, FB_BLANK_POWERDOWN = 4.
    write_file(dir_ + "/bl_power", on ? "0" : "4");
    if (on) set_percent(percent_);
}

}  // namespace facet::platform
