#include "platform/evdev_touch.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"

namespace facet::platform {

namespace {
bool test_bit(const unsigned long* bits, int bit) {
    constexpr int kBits = sizeof(unsigned long) * 8;
    return (bits[bit / kBits] >> (bit % kBits)) & 1;
}
}  // namespace

EvdevTouch::~EvdevTouch() {
    for (auto& d : devices_) ::close(d.fd);
}

void EvdevTouch::open_devices(int screen_w, int screen_h) {
    sw_ = screen_w;
    sh_ = screen_h;
    if (const char* t = std::getenv("FACET_TOUCH_TRANSFORM")) {
        std::string s = t;
        swap_ = s.find("swap") != std::string::npos;
        inv_x_ = s.find("invx") != std::string::npos;
        inv_y_ = s.find("invy") != std::string::npos;
    }
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long ev[EV_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long abs[ABS_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        if (!test_bit(ev, EV_ABS)) {
            ::close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        Device d;
        d.fd = fd;
        d.mt = test_bit(abs, ABS_MT_POSITION_X);
        int ax = d.mt ? ABS_MT_POSITION_X : ABS_X, ay = d.mt ? ABS_MT_POSITION_Y : ABS_Y;
        if (!test_bit(abs, ax) || !test_bit(abs, ay)) {
            ::close(fd);
            continue;
        }
        input_absinfo ix{}, iy{};
        ioctl(fd, EVIOCGABS(ax), &ix);
        ioctl(fd, EVIOCGABS(ay), &iy);
        d.min_x = ix.minimum;
        d.max_x = std::max(ix.maximum, ix.minimum + 1);
        d.min_y = iy.minimum;
        d.max_y = std::max(iy.maximum, iy.minimum + 1);
        unsigned long props[INPUT_PROP_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGPROP(sizeof props), props);
        d.direct = test_bit(props, INPUT_PROP_DIRECT);
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (announced_.insert(path + name).second)
            log::info("touch: %s (%s, %s, %s)", path.c_str(), name, d.mt ? "multitouch" : "single",
                      d.direct ? "touchscreen" : "touchpad");
        devices_.push_back(d);
    }
    closedir(dir);
    // A real touchscreen wins: touchpads would otherwise inject stray taps.
    bool has_direct = std::any_of(devices_.begin(), devices_.end(), [](const Device& d) { return d.direct; });
    if (has_direct) {
        for (auto& d : devices_)
            if (!d.direct) ::close(d.fd), d.fd = -1;
        devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.fd < 0; }),
                       devices_.end());
    }
    if (devices_.empty() && announced_.insert("<none>").second) log::warn("touch: no touchscreen found, will rescan");
}

void EvdevTouch::add_poll_fds(std::vector<pollfd>& fds) {
    for (auto& d : devices_) fds.push_back({d.fd, POLLIN, 0});
}

Event EvdevTouch::map(const Device& d, EventType type) const {
    float nx = float(d.raw_x - d.min_x) / float(d.max_x - d.min_x);
    float ny = float(d.raw_y - d.min_y) / float(d.max_y - d.min_y);
    if (swap_) std::swap(nx, ny);
    if (inv_x_) nx = 1 - nx;
    if (inv_y_) ny = 1 - ny;
    Event e{type};
    e.x = nx * float(sw_);
    e.y = ny * float(sh_);
    return e;
}

void EvdevTouch::read_device(Device& d, std::vector<Event>& out) {
    input_event ev[64];
    while (true) {
        ssize_t n = ::read(d.fd, ev, sizeof ev);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) {  // unplugged
                ::close(d.fd);
                d.fd = -1;
            }
            return;
        }
        for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i) {
            const input_event& e = ev[i];
            if (e.type == EV_ABS) {
                if (e.code == ABS_MT_SLOT) d.slot = e.value;
                if (d.mt && d.slot != 0) continue;
                if (e.code == ABS_MT_POSITION_X || (!d.mt && e.code == ABS_X)) d.raw_x = e.value, d.moved = true;
                if (e.code == ABS_MT_POSITION_Y || (!d.mt && e.code == ABS_Y)) d.raw_y = e.value, d.moved = true;
                if (e.code == ABS_MT_TRACKING_ID) d.down = e.value >= 0;
            } else if (e.type == EV_KEY && e.code == BTN_TOUCH && !d.mt) {
                d.down = e.value != 0;
            } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                if (d.down && !d.was_down) out.push_back(map(d, EventType::Down));
                else if (!d.down && d.was_down) out.push_back(map(d, EventType::Up));
                else if (d.down && d.moved) out.push_back(map(d, EventType::Move));
                d.was_down = d.down;
                d.moved = false;
            }
        }
    }
}

void EvdevTouch::pump(std::vector<Event>& out, double now) {
    for (auto& d : devices_) read_device(d, out);
    size_t before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.fd < 0; }),
                   devices_.end());
    // Hotplug without udev: rescan every few seconds until a touchscreen shows up.
    bool only_indirect = std::none_of(devices_.begin(), devices_.end(), [](const Device& d) { return d.direct; });
    if ((only_indirect || devices_.size() != before) && now - last_scan_ > 5) {
        last_scan_ = now;
        for (auto& d : devices_) ::close(d.fd);
        devices_.clear();
        open_devices(sw_, sh_);
    }
}

}  // namespace facet::platform
