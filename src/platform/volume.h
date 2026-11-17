// System volume through the ALSA mixer (/dev/snd/controlC*), talking to the
// kernel directly: no libasound, so it works in the static build too.
// Picks a playback volume control (Master, then PCM, Speaker, Headphone, any
// other "... Playback Volume"); FACET_VOLUME_CONTROL=<name> or
// <card>:<name> picks one by hand (FACET_DEBUG_AUDIO lists them).
#pragma once

#include <string>

namespace facet::platform {

class Volume {
public:
    bool open();
    bool available() const { return card_ >= 0; }
    const std::string& control() const { return name_; }
    // 0..100 (the control's range scaled; 0 also mutes where there is a switch).
    int get_percent() const;
    void set_percent(int percent);

private:
    int card_ = -1;
    unsigned numid_ = 0, switch_numid_ = 0;
    unsigned channels_ = 1, switch_channels_ = 1;
    long min_ = 0, max_ = 1;
    std::string name_;
};

}  // namespace facet::platform
