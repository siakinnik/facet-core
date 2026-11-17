#include "platform/volume.h"

#include <fcntl.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace facet::platform {

namespace {

struct Control {
    unsigned numid = 0;
    std::string name;
    int type = 0;
    unsigned count = 0;
    long min = 0, max = 0;
};

class Fd {
public:
    explicit Fd(int card, int flags = O_RDONLY) {
        std::string path = "/dev/snd/controlC" + std::to_string(card);
        fd = ::open(path.c_str(), flags | O_CLOEXEC);
    }
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    int fd;
};

// The mixer controls of a card (only what we need of each).
std::vector<Control> list_controls(int fd) {
    std::vector<Control> out;
    snd_ctl_elem_list list{};
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 || list.count == 0) return out;
    std::vector<snd_ctl_elem_id> ids(list.count);
    list.space = list.count;
    list.pids = ids.data();
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0) return out;
    for (unsigned i = 0; i < list.used; ++i) {
        if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER) continue;
        snd_ctl_elem_info info{};
        info.id.numid = ids[i].numid;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) != 0) continue;
        Control c;
        c.numid = ids[i].numid;
        c.name = reinterpret_cast<const char*>(ids[i].name);
        c.type = int(info.type);
        c.count = info.count;
        if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            c.min = info.value.integer.min;
            c.max = info.value.integer.max;
        }
        out.push_back(c);
    }
    return out;
}

bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Lower is better; -1: not a playback volume.
int rank(const Control& c) {
    if (c.type != SNDRV_CTL_ELEM_TYPE_INTEGER || c.max <= c.min || c.count == 0) return -1;
    static const char* const kPreferred[] = {"Master Playback Volume", "PCM Playback Volume",
                                             "Speaker Playback Volume", "Headphone Playback Volume"};
    for (int i = 0; i < 4; ++i)
        if (c.name == kPreferred[i]) return i;
    if (ends_with(c.name, "Playback Volume")) return 10;
    return -1;
}

}  // namespace

bool Volume::open() {
    card_ = -1;
    const bool debug = std::getenv("FACET_DEBUG_AUDIO") != nullptr;
    std::string want;
    int want_card = -1;
    if (const char* w = std::getenv("FACET_VOLUME_CONTROL")) {
        want = w;
        size_t colon = want.find(':');
        if (colon != std::string::npos && colon > 0 &&
            std::all_of(want.begin(), want.begin() + long(colon), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            want_card = std::atoi(want.substr(0, colon).c_str());
            want = want.substr(colon + 1);
        }
    }
    int best_rank = 1 << 30;
    Control best;
    int best_card = -1;
    std::vector<Control> best_card_controls;
    std::string seen;  // control names, for the log when nothing fits
    for (int card = 0; card < 8; ++card) {
        if (want_card >= 0 && card != want_card) continue;
        Fd f(card);
        if (f.fd < 0) continue;
        std::vector<Control> controls = list_controls(f.fd);
        for (const auto& c : controls) {
            if (debug)
                log::info("audio: card %d control %u \"%s\" (type %d, %u channels, %ld..%ld)", card, c.numid,
                          c.name.c_str(), c.type, c.count, c.min, c.max);
            if (seen.size() < 400) seen += (seen.empty() ? "" : ", ") + c.name;
            int r = want.empty() ? rank(c)
                                 : (c.name == want && c.type == SNDRV_CTL_ELEM_TYPE_INTEGER && c.max > c.min ? 0 : -1);
            if (r >= 0 && r < best_rank) {
                best_rank = r;
                best = c;
                best_card = card;
                best_card_controls = controls;
            }
        }
    }
    if (best_card < 0) {
        if (!want.empty()) log::warn("audio: no volume control \"%s\"", want.c_str());
        else if (!seen.empty())
            log::info("audio: no playback volume among the mixer controls (%s); set FACET_VOLUME_CONTROL",
                      seen.c_str());
        else log::info("audio: no sound card mixer");
        return false;
    }
    card_ = best_card;
    numid_ = best.numid;
    channels_ = best.count;
    min_ = best.min;
    max_ = best.max;
    name_ = best.name;
    // "X Playback Volume" often has "X Playback Switch" beside it: 0% mutes.
    switch_numid_ = 0;
    if (ends_with(name_, "Volume")) {
        std::string sw = name_.substr(0, name_.size() - 6) + "Switch";
        for (const auto& c : best_card_controls)
            if (c.name == sw && c.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
                switch_numid_ = c.numid;
                switch_channels_ = c.count;
            }
    }
    log::info("audio: volume control \"%s\" on card %d (%ld..%ld)%s", name_.c_str(), card_, min_, max_,
              switch_numid_ ? ", with a mute switch" : "");
    return true;
}

int Volume::get_percent() const {
    if (card_ < 0) return 0;
    Fd f(card_);
    if (f.fd < 0) return 0;
    snd_ctl_elem_value v{};
    v.id.numid = numid_;
    if (ioctl(f.fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) != 0) return 0;
    long raw = v.value.integer.value[0];
    if (switch_numid_) {
        snd_ctl_elem_value s{};
        s.id.numid = switch_numid_;
        if (ioctl(f.fd, SNDRV_CTL_IOCTL_ELEM_READ, &s) == 0 && !s.value.integer.value[0]) return 0;
    }
    return int(std::lround(double(raw - min_) * 100.0 / double(max_ - min_)));
}

void Volume::set_percent(int percent) {
    if (card_ < 0) return;
    percent = std::clamp(percent, 0, 100);
    Fd f(card_, O_RDWR);
    if (f.fd < 0) {
        log::warn("audio: control device: %s", std::strerror(errno));
        return;
    }
    snd_ctl_elem_value v{};
    v.id.numid = numid_;
    long raw = min_ + std::lround(double(max_ - min_) * percent / 100.0);
    for (unsigned i = 0; i < channels_ && i < 128; ++i) v.value.integer.value[i] = raw;
    if (ioctl(f.fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) != 0)
        log::warn("audio: setting \"%s\": %s", name_.c_str(), std::strerror(errno));
    if (switch_numid_) {
        snd_ctl_elem_value s{};
        s.id.numid = switch_numid_;
        for (unsigned i = 0; i < switch_channels_ && i < 128; ++i) s.value.integer.value[i] = percent > 0;
        ioctl(f.fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &s);
    }
}

}  // namespace facet::platform
