#include "platform/evdev_touch.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
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
    debug_ = std::getenv("FACET_DEBUG_INPUT") != nullptr;
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long ev[EV_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long abs[ABS_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long keys[KEY_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        if (!test_bit(ev, EV_ABS)) {
            ::close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        Device d;
        d.fd = fd;
        d.name = e->d_name;
        d.mt = test_bit(abs, ABS_MT_POSITION_X);
        int ax = d.mt ? ABS_MT_POSITION_X : ABS_X, ay = d.mt ? ABS_MT_POSITION_Y : ABS_Y;
        if (!test_bit(abs, ax) || !test_bit(abs, ay)) {
            ::close(fd);
            continue;
        }
        unsigned long props[INPUT_PROP_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGPROP(sizeof props), props);
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        // Axes alone are not a touch device: accelerometers, light and
        // proximity sensors and joysticks report ABS_X/Y too, without a touch
        // button or a pointer property.
        if (test_bit(ev, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        bool touch_key = test_bit(ev, EV_KEY) && (test_bit(keys, BTN_TOUCH) || test_bit(keys, BTN_TOOL_FINGER));
        if (!touch_key && !test_bit(props, INPUT_PROP_DIRECT) && !test_bit(props, INPUT_PROP_POINTER)) {
            if (debug_ && announced_.insert(path + name).second)
                log::info("touch: skipping %s (%s): no touch button or pointer property", path.c_str(), name);
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
        d.direct = test_bit(props, INPUT_PROP_DIRECT);
        d.vkeys = load_virtual_keys(name);
        if (announced_.insert(path + name).second)
            log::info("touch: %s (%s, %s, %s%s)", path.c_str(), name, d.mt ? "multitouch" : "single",
                      d.direct ? "touchscreen" : "touchpad",
                      d.vkeys.empty() ? "" : (", " + std::to_string(d.vkeys.size()) + " virtual keys").c_str());
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

// Lines or ':'-separated groups of six: 0x01:<key code>:<centre x>:<centre y>:<width>:<height>,
// in the panel's pixels.
std::vector<EvdevTouch::VirtualKey> EvdevTouch::load_virtual_keys(const std::string& device_name) const {
    std::vector<VirtualKey> keys;
    std::string path = "/sys/board_properties/virtualkeys." + device_name;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return keys;
    std::string text;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ':';
    std::vector<long> v;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(':', pos);
        if (end == std::string::npos) end = text.size();
        std::string field = text.substr(pos, end - pos);
        if (!field.empty()) v.push_back(std::strtol(field.c_str(), nullptr, 0));
        pos = end + 1;
    }
    for (size_t i = 0; i + 6 <= v.size(); i += 6) {
        if (v[i] != 1) continue;
        Key k = key_from_code(int(v[i + 1]), false);
        if (k == Key::Unknown) continue;
        float cx = float(v[i + 2]), cy = float(v[i + 3]), w = float(v[i + 4]), h = float(v[i + 5]);
        keys.push_back({k, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2});
        if (debug_)
            log::info("touch: virtual key %s at %.0f,%.0f %.0fx%.0f", key_name(k), double(cx), double(cy), double(w),
                      double(h));
    }
    return keys;
}

// Hit test before the screen transform: the boxes are in the panel's own
// coordinates, where the raw touch range spans the panel.
Key EvdevTouch::virtual_key_at(const Device& d) const {
    float x = float(d.raw_x - d.min_x) / float(d.max_x - d.min_x) * float(sw_);
    float y = float(d.raw_y - d.min_y) / float(d.max_y - d.min_y) * float(sh_);
    if (swap_) {  // a panel mounted turned: its own width is our height
        x = float(d.raw_x - d.min_x) / float(d.max_x - d.min_x) * float(sh_);
        y = float(d.raw_y - d.min_y) / float(d.max_y - d.min_y) * float(sw_);
    }
    for (const auto& k : d.vkeys)
        if (x >= k.x0 && x < k.x1 && y >= k.y0 && y < k.y1) return k.key;
    return Key::Unknown;
}

void EvdevTouch::set_proto(Device& d, Proto p) const {
    if (d.proto == p || !d.mt) return;
    d.proto = p;
    if (debug_) log::info("touch: %s uses MT protocol %s", d.name.c_str(), p == Proto::A ? "A" : "B");
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
                if (e.code == ABS_MT_SLOT || e.code == ABS_MT_TRACKING_ID) set_proto(d, Proto::B);
                if (e.code == ABS_MT_SLOT) d.slot = e.value;
                if (!d.mt) {
                    if (e.code == ABS_X) d.raw_x = e.value, d.moved = true;
                    if (e.code == ABS_Y) d.raw_y = e.value, d.moved = true;
                } else if (d.proto == Proto::B) {
                    if (d.slot != 0) continue;
                    if (e.code == ABS_MT_POSITION_X) d.raw_x = e.value, d.moved = true;
                    if (e.code == ABS_MT_POSITION_Y) d.raw_y = e.value, d.moved = true;
                    if (e.code == ABS_MT_TRACKING_ID) d.down = e.value >= 0;
                } else if (e.code == ABS_MT_POSITION_X || e.code == ABS_MT_POSITION_Y) {
                    // Protocol A: the first contact of the frame is the finger we follow.
                    d.contact_pos = d.frame_mt = true;
                    if (d.contact != 0) continue;
                    (e.code == ABS_MT_POSITION_X ? d.raw_x : d.raw_y) = e.value;
                    d.moved = true;
                }
            } else if (e.type == EV_KEY && (e.code == BTN_TOUCH || e.code == BTN_TOOL_FINGER)) {
                if (!d.mt) {
                    if (e.code == BTN_TOUCH) d.down = e.value != 0;
                } else {
                    (e.code == BTN_TOUCH ? d.btn_touch : d.btn_tool) = e.value != 0;
                    d.btn_known = d.btn_event = true;
                }
            } else if (e.type == EV_SYN && e.code == SYN_MT_REPORT) {
                if (d.proto == Proto::Unknown) set_proto(d, Proto::A);
                if (d.contact_pos) ++d.contacts;
                ++d.contact;
                d.contact_pos = false;
                d.frame_mt = true;
            } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                if (d.mt && d.proto != Proto::B) {
                    // A frame without any contact is a release, BTN_TOUCH 0 or not
                    // (some drivers never send it); a released button is one too.
                    if (d.contact_pos) ++d.contacts;  // the last contact had no SYN_MT_REPORT
                    if (d.frame_mt || d.btn_event)
                        d.down = d.contacts > 0 && !(d.btn_known && !d.btn_touch && !d.btn_tool);
                    d.contact = d.contacts = 0;
                    d.contact_pos = d.frame_mt = d.btn_event = false;
                }
                if (d.down && !d.was_down) {
                    d.vkey = d.vkeys.empty() ? Key::Unknown : virtual_key_at(d);
                    Event k{EventType::KeyDown};
                    k.key = d.vkey;
                    out.push_back(d.vkey != Key::Unknown ? k : map(d, EventType::Down));
                } else if (!d.down && d.was_down) {
                    Event k{EventType::KeyUp};
                    k.key = d.vkey;
                    out.push_back(d.vkey != Key::Unknown ? k : map(d, EventType::Up));
                    d.vkey = Key::Unknown;
                } else if (d.down && d.moved && d.vkey == Key::Unknown) {
                    out.push_back(map(d, EventType::Move));
                }
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
