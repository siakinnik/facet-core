#include "platform/evdev_keys.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace facet::platform {

namespace {

bool test_bit(const unsigned long* bits, int bit) {
    constexpr int kBits = sizeof(unsigned long) * 8;
    return (bits[bit / kBits] >> (bit % kBits)) & 1;
}

constexpr double kRescanS = 10;
constexpr int kCodes[] = {KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE, KEY_POWER, KEY_BACK,
                          KEY_HOMEPAGE, KEY_HOME,       KEY_MENU, KEY_APPSELECT};

}  // namespace

EvdevKeys::~EvdevKeys() { close_all(); }

void EvdevKeys::close_all() {
    for (auto& d : devices_)
        if (d.fd >= 0) {
            ioctl(d.fd, EVIOCGRAB, 0);
            ::close(d.fd);
        }
    devices_.clear();
}

void EvdevKeys::open_devices() {
    close_all();
    const char* off = std::getenv("FACET_KEYS");
    if (off && std::strcmp(off, "off") == 0) return;
    debug_ = std::getenv("FACET_DEBUG_INPUT") != nullptr;
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    std::vector<std::string> paths;
    while (dirent* e = readdir(dir))
        if (std::strncmp(e->d_name, "event", 5) == 0) paths.push_back(std::string("/dev/input/") + e->d_name);
    closedir(dir);
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ev[EV_MAX / (sizeof(long) * 8) + 1] = {};
        unsigned long keys[KEY_MAX / (sizeof(long) * 8) + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        if (test_bit(ev, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        // A keyboard has letters: its Home and Menu keys are not navigation.
        bool keyboard = test_bit(keys, KEY_A) && test_bit(keys, KEY_Z);
        std::string found;
        for (int code : kCodes)
            if (test_bit(keys, code) && key_from_code(code, keyboard) != Key::Unknown) {
                if (!found.empty()) found += ", ";
                found += key_name(key_from_code(code, keyboard));
            }
        if (found.empty()) {
            ::close(fd);
            continue;
        }
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        // Buttons alone (no touchscreen, mouse or keyboard on the same device)
        // are ours only: a grab keeps logind from shutting down on the power key.
        // A touchscreen that also reports its keys is left shared: the touch
        // reader opens it too.
        bool grab = !test_bit(ev, EV_ABS) && !test_bit(ev, EV_REL) && !keyboard;
        if (grab && ioctl(fd, EVIOCGRAB, 1) != 0) grab = false;
        if (announced_.insert(path + name).second)
            log::info("keys: %s (%s): %s%s", path.c_str(), name, found.c_str(), grab ? "" : ", shared");
        devices_.push_back({fd, path, keyboard});
    }
}

void EvdevKeys::add_poll_fds(std::vector<pollfd>& fds) const {
    for (const auto& d : devices_) fds.push_back({d.fd, POLLIN, 0});
}

void EvdevKeys::read_device(Device& d, std::vector<Event>& out) {
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
            if (e.type != EV_KEY) continue;
            Key k = key_from_code(e.code, d.keyboard);
            if (k == Key::Unknown) continue;
            if (debug_ && e.value != 2)
                log::info("keys: %s %s (code %d)", key_name(k), e.value ? "down" : "up", int(e.code));
            Event out_e{e.value ? EventType::KeyDown : EventType::KeyUp};
            out_e.key = k;
            out_e.repeat = e.value == 2;
            out.push_back(out_e);
        }
    }
}

void EvdevKeys::pump(std::vector<Event>& out, double now) {
    for (auto& d : devices_) read_device(d, out);
    size_t before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.fd < 0; }),
                   devices_.end());
    // Built-in buttons are there from the start; look again only for a
    // device that went away or while there is none.
    if ((devices_.empty() || devices_.size() != before) && now - last_scan_ >= kRescanS) {
        last_scan_ = now;
        open_devices();
    }
}

}  // namespace facet::platform
