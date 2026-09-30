// Network status for the status bar, gathered without root: interface state
// from /sys/class/net, Wi-Fi signal from /proc/net/wireless and the SSID from
// nl80211 (falling back to `iw`, `iwgetid` or `nmcli`). Polled on a
// background thread so a slow tool never stalls the UI.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace facet::net {

enum class Kind { Offline, Ethernet, Wifi };

struct Status {
    Kind kind = Kind::Offline;
    std::string iface;  // "wlan0"
    std::string ssid;   // "" if unknown
    int signal = -1;    // Wi-Fi quality 0..100, -1 if unknown
    std::string ipv4;   // "" if none
    bool operator==(const Status& o) const {
        return kind == o.kind && iface == o.iface && ssid == o.ssid && signal == o.signal && ipv4 == o.ipv4;
    }
};

class Monitor {
public:
    Monitor();
    ~Monitor();
    Status status() const;
    // True once after the status changed (for triggering a redraw).
    bool take_changed() { return changed_.exchange(false); }

private:
    void run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    Status status_;
    std::atomic<bool> changed_{true};
    bool stop_ = false;
    std::thread thread_;
};

// Signal quality 0..100 -> 0..3 bars.
int bars(int signal);

}  // namespace facet::net
