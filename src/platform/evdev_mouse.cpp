#include "platform/evdev_mouse.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "core/log.h"

namespace facet::platform {

namespace {

bool test_bit(const unsigned long* bits, int bit) {
    constexpr int kBits = sizeof(unsigned long) * 8;
    return (bits[bit / kBits] >> (bit % kBits)) & 1;
}

constexpr double kRescanS = 4;

}  // namespace

EvdevMouse::~EvdevMouse() { close_all(); }

std::vector<std::string> EvdevMouse::scan() const {
    std::vector<std::string> found;
    DIR* dir = opendir("/dev/input");
    if (!dir) return found;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ev[EV_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long rel[REL_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long key[KEY_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        bool mouse = false;
        if (test_bit(ev, EV_REL) && test_bit(ev, EV_KEY)) {
            ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
            mouse = test_bit(rel, REL_X) && test_bit(rel, REL_Y) && test_bit(key, BTN_LEFT);
        }
        ::close(fd);
        if (mouse) found.push_back(path);
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    return found;
}

void EvdevMouse::open_all() {
    close_all();
    for (const auto& path : scan()) {
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (announced_.insert(path + name).second) log::info("mouse: %s (%s)", path.c_str(), name);
        devices_.push_back({fd, path});
    }
}

void EvdevMouse::close_all() {
    for (auto& d : devices_)
        if (d.fd >= 0) ::close(d.fd);
    devices_.clear();
}

void EvdevMouse::set_enabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    last_scan_ = -100;
    if (on) open_all();
    else close_all();
    down_ = false;
}

void EvdevMouse::add_poll_fds(std::vector<pollfd>& fds) const {
    for (const auto& d : devices_) fds.push_back({d.fd, POLLIN, 0});
}

void EvdevMouse::read_device(Device& d, std::vector<Event>& out, int w, int h) {
    input_event ev[64];
    float dx = 0, dy = 0, wheel = 0;
    bool button_changed = false;
    bool pressed = down_;
    auto flush = [&] {
        if (dx != 0 || dy != 0) {
            x_ = std::clamp(x_ + dx, 0.f, float(w - 1));
            y_ = std::clamp(y_ + dy, 0.f, float(h - 1));
            Event m{EventType::Move};
            m.x = x_, m.y = y_, m.mouse = true;
            out.push_back(m);
        }
        if (button_changed) {
            Event b{pressed ? EventType::Down : EventType::Up};
            b.x = x_, b.y = y_, b.mouse = true;
            out.push_back(b);
            down_ = pressed;
        }
        if (wheel != 0) {
            Event s{EventType::Wheel};
            s.x = x_, s.y = y_, s.dy = wheel, s.mouse = true;
            out.push_back(s);
        }
        dx = dy = wheel = 0;
        button_changed = false;
    };
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
            if (e.type == EV_REL) {
                if (e.code == REL_X) dx += float(e.value);
                else if (e.code == REL_Y) dy += float(e.value);
                else if (e.code == REL_WHEEL) wheel -= float(e.value);  // wheel up = content down
            } else if (e.type == EV_KEY && e.code == BTN_LEFT) {
                if ((e.value != 0) != pressed) {
                    pressed = e.value != 0;
                    button_changed = true;
                }
            } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                flush();
            }
        }
    }
}

void EvdevMouse::pump(std::vector<Event>& out, double now, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x_ < 0) x_ = float(w) / 2, y_ = float(h) / 2;
    for (auto& d : devices_) read_device(d, out, w, h);
    size_t before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.fd < 0; }),
                   devices_.end());
    if (now - last_scan_ >= kRescanS || devices_.size() != before) {
        last_scan_ = now;
        std::vector<std::string> now_found = scan();
        available_ = !now_found.empty();
        if (enabled_) {
            // A mouse that came or went: open what is new.
            bool same = now_found.size() == devices_.size();
            if (same)
                for (size_t i = 0; i < now_found.size(); ++i) same = same && devices_[i].path == now_found[i];
            if (!same) open_all();
        }
    }
}

}  // namespace facet::platform
