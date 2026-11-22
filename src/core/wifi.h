// Wi-Fi: scanning, connecting, saved networks. Two ways to drive it, picked
// by what runs on the system:
// - NetworkManager, through nmcli (passwords go in on its standard input,
//   never on a command line other processes can read);
// - wpa_supplicant alone (systemd-networkd, ifupdown, dhcpcd setups), through
//   its control socket. Facet keeps the networks it added in its config (the
//   WPA key derived from the password, not the password itself) and adds them
//   again after a restart, since such systems often do not let wpa_supplicant
//   save its configuration. Getting an address stays the job of the network
//   service (networkd, dhcpcd, ...).
// Everything runs on a worker thread; wake_fd() becomes readable when the
// state changed.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "facet/json.h"

namespace facet::net {

struct WifiNetwork {
    std::string ssid;
    int signal = 0;           // 0..100
    bool secured = false;     // needs a password
    bool enterprise = false;  // 802.1X (a user name and a certificate): not set up here
    bool saved = false;       // connects without asking for the password
    bool current = false;
};

class Wifi {
public:
    enum class Backend { Detecting, None, NetworkManager, WpaSupplicant };
    enum class Phase { Idle, Connecting, Connected, Failed, WrongPassword };
    struct State {
        Backend backend = Backend::Detecting;
        std::string iface;
        bool enabled = true;  // the radio is on
        bool can_toggle = false;
        bool can_power = false;  // no adapter yet, but the chip can be switched on (MediaTek)
        bool has_wpa = false;    // wpa_supplicant is installed
        bool scanning = false;
        std::vector<WifiNetwork> networks;  // the current one first, then by signal
        std::string current;                // SSID connected now, "" if none
        Phase phase = Phase::Idle;          // of the last connect()
        std::string target;                 // SSID of the last connect()
        std::string error;                  // what the tool said when it failed
    };

    ~Wifi();
    // `saved`: the wpa_supplicant networks Facet added before (config "wifi_networks").
    void start(const Json& saved);
    State state() const;
    int wake_fd() const { return wake_[0]; }
    // True once after the state changed; also empties wake_fd().
    bool take_changed();
    // wpa_supplicant: the networks to keep in the config, when they changed.
    std::optional<Json> take_saved();

    void scan();
    void set_enabled(bool on);
    // `password` empty: an open or a saved network.
    void connect(const std::string& ssid, const std::string& password);
    void disconnect();
    void forget(const std::string& ssid);

    // The WPA key wpa_supplicant derives from a passphrase (PBKDF2-HMAC-SHA1,
    // 4096 rounds, the SSID as salt), as 64 hex digits.
    static std::string wpa_psk(const std::string& ssid, const std::string& passphrase);

private:
    void run();
    void post(std::function<void()> job);
    void notify();  // the state changed (worker thread)
    void detect();
    bool start_own_wpa(const std::string& iface);
    void dhcp(const std::string& iface);
    void refresh(bool rescan);
    // NetworkManager.
    void nm_refresh(bool rescan);
    void nm_connect(const std::string& ssid, const std::string& password);
    // wpa_supplicant.
    std::string wpa(const std::string& cmd, int timeout_ms = 3000);
    void wpa_refresh(bool rescan);
    void wpa_connect(const std::string& ssid, const std::string& password);
    void wpa_restore_saved();
    bool rfkill_blocked(bool& found) const;
    bool rfkill_set(bool blocked) const;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    State state_;
    bool changed_ = true;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::thread thread_;
    int wake_[2] = {-1, -1};

    // Worker thread only.
    std::string ctrl_path_;  // wpa_supplicant control socket
    std::string local_path_;
    int ctrl_fd_ = -1;
    bool own_wpa_ = false;  // wpa_supplicant started by Facet: it also gets the address
    double last_dhcp_ = -1000;
    std::string logged_;    // what detection said last
    Json saved_ = Json::array();  // under mu_ when read by take_saved()
    bool saved_changed_ = false;
};

}  // namespace facet::net
