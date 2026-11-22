#include "core/wifi.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <ifaddrs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

#include "core/log.h"

namespace facet::net {

namespace {

// ------------------------------------------------------------------ SHA-1 / PBKDF2

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t bits = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void update(const uint8_t* p, size_t n) {
        bits += uint64_t(n) * 8;
        while (n > 0) {
            size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take, p += take, n -= take;
            if (used == 64) block(buf), used = 0;
        }
    }
    void finish(uint8_t out[20]) {
        uint64_t total = bits;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(total >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
    }
};

void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint8_t k[64] = {};
    if (key_len > 64) {
        Sha1 s;
        s.update(key, key_len);
        s.finish(k);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    uint8_t inner[20];
    Sha1 a;
    a.update(ipad, 64);
    a.update(msg, len);
    a.finish(inner);
    Sha1 b;
    b.update(opad, 64);
    b.update(inner, 20);
    b.finish(out);
}

// ------------------------------------------------------------------ helpers

double mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) out += d[c >> 4], out += d[c & 15];
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t e = s.find(sep, pos);
        out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}

// nmcli -t: fields separated by ':', with '\:' and '\\' escaped.
std::vector<std::string> nm_fields(const std::string& line) {
    std::vector<std::string> out(1);
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) out.back() += line[++i];
        else if (line[i] == ':') out.emplace_back();
        else out.back() += line[i];
    }
    return out;
}

// wpa_supplicant prints SSIDs escaped: \" \\ \e \n \r \t \xNN.
std::string wpa_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char c = s[++i];
        if (c == 'n') out += '\n';
        else if (c == 'r') out += '\r';
        else if (c == 't') out += '\t';
        else if (c == 'e') out += '\033';
        else if (c == 'x' && i + 2 < s.size()) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else out += c;
    }
    return out;
}

struct RunResult {
    int status = -1;  // exit code, -1 if it did not run or timed out
    std::string out;  // stdout and stderr together
};

// Runs a program (no shell) with `input` on its stdin, for at most `timeout` s.
RunResult exec_cmd(const std::vector<std::string>& argv, const std::string& input = {}, double timeout = 40) {
    RunResult r;
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0) return r;
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]), close(in[1]);
        return r;
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        setenv("LC_ALL", "C", 1);  // messages the code below can recognise
        execvp(args[0], args.data());
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]), close(out[0]);
        return r;
    }
    if (!input.empty()) {
        ssize_t n = write(in[1], input.data(), input.size());
        (void)n;
    }
    close(in[1]);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    double end = mono() + timeout;
    char buf[4096];
    while (true) {
        double left = end - mono();
        if (left <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        pollfd p{out[0], POLLIN, 0};
        if (poll(&p, 1, int(left * 1000) + 1) <= 0) continue;
        ssize_t n = read(out[0], buf, sizeof buf);
        if (n > 0) r.out.append(buf, size_t(n));
        else if (n == 0 || errno != EAGAIN) break;
    }
    close(out[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(st) && mono() < end) r.status = WEXITSTATUS(st);
    if (r.status == 127) r.status = -1;  // not installed
    return r;
}

std::string first_line(const std::string& s) {
    size_t a = s.find_first_not_of(" \n");
    if (a == std::string::npos) return {};
    size_t e = s.find('\n', a);
    std::string line = s.substr(a, e == std::string::npos ? std::string::npos : e - a);
    if (line.rfind("Error: ", 0) == 0) line = line.substr(7);
    return line;
}

// Wireless interfaces (/sys/class/net/<if>/phy80211 or wireless): "wl*"
// names first, VLANs on top of them ("iot0.10") left out.
std::vector<std::string> wireless_ifaces() {
    std::vector<std::string> out;
    if (DIR* d = opendir("/sys/class/net")) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n[0] == '.' || n.find('.') != std::string::npos) continue;
            std::string base = "/sys/class/net/" + n;
            struct stat st;
            if (stat((base + "/wireless").c_str(), &st) == 0 || stat((base + "/phy80211").c_str(), &st) == 0)
                out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        bool wa = a.rfind("wl", 0) == 0, wb = b.rfind("wl", 0) == 0;
        return wa != wb ? wa : a < b;
    });
    return out;
}

bool is_socket(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// A program in PATH or the usual system directories, "" if none.
std::string find_program(const std::string& name) {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":/sbin:/usr/sbin:/bin:/usr/bin:/system/bin:/vendor/bin";
    for (const auto& dir : split(path, ':')) {
        if (dir.empty()) continue;
        std::string p = dir + "/" + name;
        if (access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

bool has_ipv4(const std::string& iface) {
    bool found = false;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    for (ifaddrs* a = list; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && iface == a->ifa_name) found = true;
    freeifaddrs(list);
    return found;
}

// MediaTek combo chips: the Wi-Fi part is powered by writing to this device,
// and only then does wlan0 appear.
constexpr const char* kMtkPower = "/dev/wmtWifi";
// Where Facet's own wpa_supplicant (on systems that run none) keeps its socket.
constexpr const char* kOwnCtrlDir = "/run/facet-wpa";

int quality_from_dbm(int dbm) { return std::clamp(2 * (dbm + 100), 0, 100); }

void sort_networks(std::vector<WifiNetwork>& v) {
    std::stable_sort(v.begin(), v.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
        if (a.current != b.current) return a.current;
        if (a.saved != b.saved) return a.saved;
        return a.signal > b.signal;
    });
}

}  // namespace

std::string Wifi::wpa_psk(const std::string& ssid, const std::string& passphrase) {
    std::string key;
    const auto* pw = reinterpret_cast<const uint8_t*>(passphrase.data());
    for (uint32_t block = 1; key.size() < 32; ++block) {
        std::string salt = ssid;
        salt += char(block >> 24), salt += char(block >> 16), salt += char(block >> 8), salt += char(block);
        uint8_t u[20], t[20];
        hmac_sha1(pw, passphrase.size(), reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), u);
        std::memcpy(t, u, 20);
        for (int i = 1; i < 4096; ++i) {
            hmac_sha1(pw, passphrase.size(), u, 20, u);
            for (int j = 0; j < 20; ++j) t[j] ^= u[j];
        }
        key.append(reinterpret_cast<char*>(t), 20);
    }
    return hex(key.substr(0, 32));
}

// ------------------------------------------------------------------ thread

Wifi::~Wifi() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (ctrl_fd_ >= 0) close(ctrl_fd_);
    if (!local_path_.empty()) unlink(local_path_.c_str());
    for (int fd : wake_)
        if (fd >= 0) close(fd);
}

void Wifi::start(const Json& saved) {
    if (thread_.joinable()) return;
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) wake_[0] = wake_[1] = -1;
    if (saved.is_array()) saved_ = saved;
    thread_ = std::thread([this] { run(); });
}

Wifi::State Wifi::state() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

bool Wifi::take_changed() {
    char buf[64];
    while (wake_[0] >= 0 && read(wake_[0], buf, sizeof buf) > 0) {
    }
    std::lock_guard<std::mutex> lock(mu_);
    bool c = changed_;
    changed_ = false;
    return c;
}

std::optional<Json> Wifi::take_saved() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!saved_changed_) return std::nullopt;
    saved_changed_ = false;
    return saved_;
}

void Wifi::notify() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        changed_ = true;
    }
    if (wake_[1] >= 0) {
        char c = 1;
        ssize_t n = write(wake_[1], &c, 1);
        (void)n;
    }
}

void Wifi::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void Wifi::run() {
    detect();
    refresh(false);
    double next_refresh = mono() + 20;
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(20), [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
        }
        if (job) {
            job();
        } else if (mono() >= next_refresh) {
            // Nothing found yet: look again (a driver that loads late, a chip switched on).
            Backend b = state().backend;
            if (b == Backend::None || state().iface.empty() ||
                (!state().iface.empty() && !exists("/sys/class/net/" + state().iface)))
                detect();
            refresh(false);  // e.g. connected by something else
        }
        if (!job) next_refresh = mono() + 20;
    }
}

void Wifi::scan() {
    post([this] { refresh(true); });
}

void Wifi::set_enabled(bool on) {
    post([this, on] {
        State s = state();
        bool ok = false;
        if (exists(kMtkPower) && s.backend != Backend::NetworkManager) {
            // MediaTek: the chip itself goes on and off; wlan0 comes and goes with it.
            if (FILE* f = std::fopen(kMtkPower, "w")) {
                ok = std::fputs(on ? "1" : "0", f) >= 0;
                ok = std::fclose(f) == 0 && ok;
            }
            for (int i = 0; i < 100 && on && wireless_ifaces().empty(); ++i) usleep(100 * 1000);
            detect();
        } else if (s.backend == Backend::NetworkManager) {
            ok = exec_cmd({"nmcli", "radio", "wifi", on ? "on" : "off"}).status == 0;
        } else if (s.backend == Backend::WpaSupplicant) {
            ok = rfkill_set(!on);
            if (!ok) ok = wpa(on ? "RECONNECT" : "DISCONNECT").rfind("OK", 0) == 0;
        }
        if (!ok) log::warn("wifi: could not switch %s", on ? "on" : "off");
        if (on) {
            usleep(1500 * 1000);  // the radio comes up before it can scan
            refresh(true);
        } else {
            refresh(false);
        }
    });
}

void Wifi::connect(const std::string& ssid, const std::string& password) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.phase = Phase::Connecting;
        state_.target = ssid;
        state_.error.clear();
        changed_ = true;
    }
    post([this, ssid, password] {
        Backend b = state().backend;
        if (b == Backend::NetworkManager) nm_connect(ssid, password);
        else if (b == Backend::WpaSupplicant) wpa_connect(ssid, password);
        refresh(false);
    });
}

void Wifi::disconnect() {
    post([this] {
        State s = state();
        if (s.backend == Backend::NetworkManager) exec_cmd({"nmcli", "device", "disconnect", s.iface});
        else if (s.backend == Backend::WpaSupplicant) wpa("DISCONNECT");
        refresh(false);
    });
}

void Wifi::forget(const std::string& ssid) {
    post([this, ssid] {
        State s = state();
        if (s.backend == Backend::NetworkManager) {
            exec_cmd({"nmcli", "connection", "delete", "id", ssid});
        } else if (s.backend == Backend::WpaSupplicant) {
            for (const auto& line : split(wpa("LIST_NETWORKS"), '\n')) {
                auto f = split(line, '\t');
                if (f.size() >= 2 && wpa_unescape(f[1]) == ssid) wpa("REMOVE_NETWORK " + f[0]);
            }
            wpa("SAVE_CONFIG");
            std::lock_guard<std::mutex> lock(mu_);
            Json kept = Json::array();
            for (const auto& n : saved_.items())
                if (n["ssid"].str() != ssid) kept.push_back(n);
            saved_ = kept;
            saved_changed_ = true;
        }
        refresh(false);
    });
}

// ------------------------------------------------------------------ detection

void Wifi::detect() {
    std::vector<std::string> ifaces = wireless_ifaces();
    std::string iface;
    Backend b = Backend::None;
    // FACET_WPA_CTRL: wpa_supplicant's control socket where it is not under
    // /run/wpa_supplicant (Android-style systems); the interface is its name.
    if (const char* ctrl = std::getenv("FACET_WPA_CTRL")) {
        ctrl_path_ = ctrl;
        iface = ctrl_path_.substr(ctrl_path_.rfind('/') + 1);
        b = Backend::WpaSupplicant;
    } else if (!ifaces.empty()) {
        RunResult nm = exec_cmd({"nmcli", "-t", "-f", "RUNNING", "general"}, {}, 10);
        if (nm.status == 0 && nm.out.find("running") != std::string::npos) {
            // NetworkManager may leave a device alone ("unmanaged").
            RunResult dev = exec_cmd({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"}, {}, 10);
            for (const auto& line : split(dev.out, '\n')) {
                auto f = nm_fields(line);
                if (f.size() >= 3 && f[1] == "wifi" && f[2] != "unmanaged") {
                    b = Backend::NetworkManager;
                    iface = f[0];
                    break;
                }
            }
        }
        // The interface a wpa_supplicant serves (not, say, an access point's).
        for (const auto& name : ifaces) {
            if (b != Backend::None) break;
            for (const std::string& dir : {std::string("/run/wpa_supplicant/"), std::string("/var/run/wpa_supplicant/"),
                                           std::string(kOwnCtrlDir) + "/"}) {
                if (is_socket(dir + name)) {
                    ctrl_path_ = dir + name;
                    iface = name;
                    b = Backend::WpaSupplicant;
                    own_wpa_ = dir.rfind(kOwnCtrlDir, 0) == 0;
                    break;
                }
            }
        }
        // Nothing manages Wi-Fi (a bare phone image): run wpa_supplicant ourselves.
        if (b == Backend::None && start_own_wpa(ifaces[0])) {
            iface = ifaces[0];
            b = Backend::WpaSupplicant;
        }
    }
    if (iface.empty() && !ifaces.empty()) iface = ifaces[0];
    if (ctrl_fd_ >= 0) {  // another socket now: connect afresh
        close(ctrl_fd_);
        ctrl_fd_ = -1;
    }
    const bool mtk = exists(kMtkPower);
    bool rf_found = false;
    rfkill_blocked(rf_found);
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.backend = b;
        state_.iface = iface;
        state_.can_power = ifaces.empty() && mtk;
        state_.has_wpa = !find_program("wpa_supplicant").empty();
        state_.can_toggle = b == Backend::NetworkManager || (b == Backend::WpaSupplicant && (rf_found || mtk));
        if (ifaces.empty()) state_.enabled = false;
    }
    const char* names[] = {"detecting", "none", "NetworkManager", "wpa_supplicant"};
    std::string key = iface + "/" + names[int(b)];
    if (key != logged_) {  // detection repeats while nothing is found: log changes only
        logged_ = key;
        if (iface.empty()) log::info("wifi: no wireless interface%s", mtk ? " (the MediaTek Wi-Fi chip is off)" : "");
        else log::info("wifi: %s through %s%s%s", iface.c_str(), names[int(b)], own_wpa_ ? " (started by Facet)" : "",
                       b == Backend::None ? " (neither NetworkManager nor wpa_supplicant runs for it)" : "");
    }
    if (b == Backend::WpaSupplicant) wpa_restore_saved();
    notify();
}

// Starts wpa_supplicant for `iface` with a configuration of Facet's own, so
// that Wi-Fi works on systems that bring no network service.
bool Wifi::start_own_wpa(const std::string& iface) {
    std::string bin = find_program("wpa_supplicant");
    if (bin.empty() || getuid() != 0) return false;
    mkdir(kOwnCtrlDir, 0700);
    std::string conf = std::string(kOwnCtrlDir) + "/facet.conf";
    if (FILE* f = std::fopen(conf.c_str(), "w")) {
        std::fprintf(f, "ctrl_interface=%s\nupdate_config=1\n", kOwnCtrlDir);
        std::fclose(f);
    }
    RunResult r = exec_cmd({bin, "-B", "-i", iface, "-D", "nl80211,wext", "-c", conf, "-P",
                            std::string(kOwnCtrlDir) + "/" + iface + ".pid"},
                           {}, 15);
    std::string sock = std::string(kOwnCtrlDir) + "/" + iface;
    for (int i = 0; i < 30 && !is_socket(sock); ++i) usleep(100 * 1000);
    if (!is_socket(sock)) {
        log::warn("wifi: could not start wpa_supplicant for %s: %s", iface.c_str(), first_line(r.out).c_str());
        return false;
    }
    ctrl_path_ = sock;
    own_wpa_ = true;
    log::info("wifi: started wpa_supplicant for %s", iface.c_str());
    return true;
}

// An address for the network wpa_supplicant joined, when Facet runs it (else
// the network service does that): the first DHCP client the system has.
void Wifi::dhcp(const std::string& iface) {
    last_dhcp_ = mono();
    std::vector<std::vector<std::string>> clients = {
        {"dhcpcd", "-4", "-1", "-w", iface}, {"dhclient", "-1", "-4", iface}, {"udhcpc", "-i", iface, "-q", "-n", "-t", "6"}};
    for (auto& argv : clients) {
        std::string bin = find_program(argv[0]);
        if (bin.empty()) continue;
        argv[0] = bin;
        RunResult r = exec_cmd(argv, {}, 40);
        log::info("wifi: %s on %s: %s", argv[0].c_str(), iface.c_str(),
                  r.status == 0 ? "done" : first_line(r.out).c_str());
        if (has_ipv4(iface)) return;
    }
    if (!has_ipv4(iface)) log::warn("wifi: %s has no address (no DHCP client worked)", iface.c_str());
}

void Wifi::refresh(bool rescan) {
    Backend b = state().backend;
    if (b == Backend::NetworkManager) nm_refresh(rescan);
    else if (b == Backend::WpaSupplicant) wpa_refresh(rescan);
    notify();
}

// ------------------------------------------------------------------ NetworkManager

void Wifi::nm_refresh(bool rescan) {
    std::string iface = state().iface;
    if (rescan) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_.scanning = true;
        }
        notify();
        exec_cmd({"nmcli", "device", "wifi", "rescan", "ifname", iface}, {}, 15);
    }
    RunResult radio = exec_cmd({"nmcli", "-t", "radio", "wifi"}, {}, 10);
    RunResult list = exec_cmd({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list", "ifname",
                          iface, "--rescan", "no"},
                         {}, 15);
    RunResult conns = exec_cmd({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"}, {}, 10);
    std::vector<std::string> saved;
    for (const auto& line : split(conns.out, '\n')) {
        auto f = nm_fields(line);
        if (f.size() >= 2 && f[1] == "802-11-wireless") saved.push_back(f[0]);
    }
    std::map<std::string, WifiNetwork> by_ssid;
    std::string current;
    for (const auto& line : split(list.out, '\n')) {
        auto f = nm_fields(line);
        if (f.size() < 4 || f[1].empty()) continue;
        WifiNetwork n;
        n.ssid = f[1];
        n.current = f[0] == "*";
        n.signal = std::clamp(std::atoi(f[2].c_str()), 0, 100);
        n.secured = !f[3].empty() && f[3] != "--";
        n.enterprise = f[3].find("802.1X") != std::string::npos;
        n.saved = std::find(saved.begin(), saved.end(), n.ssid) != saved.end();
        if (n.current) current = n.ssid;
        auto it = by_ssid.find(n.ssid);
        if (it == by_ssid.end()) by_ssid[n.ssid] = n;
        else {
            it->second.signal = std::max(it->second.signal, n.signal);
            it->second.current = it->second.current || n.current;
        }
    }
    std::vector<WifiNetwork> nets;
    for (auto& [ssid, n] : by_ssid) nets.push_back(n);
    sort_networks(nets);
    std::lock_guard<std::mutex> lock(mu_);
    state_.scanning = false;
    if (radio.status == 0) state_.enabled = radio.out.find("enabled") != std::string::npos;
    state_.networks = std::move(nets);
    state_.current = current;
}

void Wifi::nm_connect(const std::string& ssid, const std::string& password) {
    State s = state();
    bool was_saved = false;
    for (const auto& n : s.networks)
        if (n.ssid == ssid) was_saved = n.saved;
    RunResult r;
    if (was_saved && password.empty()) {
        r = exec_cmd({"nmcli", "--wait", "30", "connection", "up", "id", ssid});
    } else {
        if (was_saved) exec_cmd({"nmcli", "connection", "delete", "id", ssid});  // a new password replaces the old
        std::vector<std::string> argv = {"nmcli", "--wait", "30"};
        if (!password.empty()) argv.push_back("--ask");  // the password on stdin, never in argv
        for (const char* a : {"device", "wifi", "connect"}) argv.push_back(a);
        argv.push_back(ssid);
        argv.push_back("ifname");
        argv.push_back(s.iface);
        r = exec_cmd(argv, password.empty() ? std::string() : password + "\n");
    }
    Phase phase = Phase::Connected;
    std::string error;
    if (r.status != 0) {
        error = first_line(r.out);
        bool secrets = r.out.find("Secrets were required") != std::string::npos ||
                       r.out.find("802-11-wireless-security.psk") != std::string::npos ||
                       r.out.find("password") != std::string::npos;
        phase = secrets ? Phase::WrongPassword : Phase::Failed;
        // A profile NetworkManager made for a failed first attempt would keep the wrong password.
        if (!was_saved || !password.empty()) exec_cmd({"nmcli", "connection", "delete", "id", ssid});
        log::warn("wifi: connecting to %s failed: %s", ssid.c_str(), error.c_str());
    } else {
        log::info("wifi: connected to %s", ssid.c_str());
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (state_.target == ssid) {
        state_.phase = phase;
        state_.error = error;
    }
}

// ------------------------------------------------------------------ wpa_supplicant

std::string Wifi::wpa(const std::string& cmd, int timeout_ms) {
    if (ctrl_fd_ < 0) {
        ctrl_fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (ctrl_fd_ < 0) return {};
        // wpa_supplicant answers to the sender's address: a file of our own.
        static int counter = 0;
        local_path_ = "/tmp/facet-wpa-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
        unlink(local_path_.c_str());
        sockaddr_un local{};
        local.sun_family = AF_UNIX;
        std::snprintf(local.sun_path, sizeof local.sun_path, "%s", local_path_.c_str());
        sockaddr_un remote{};
        remote.sun_family = AF_UNIX;
        std::snprintf(remote.sun_path, sizeof remote.sun_path, "%s", ctrl_path_.c_str());
        if (bind(ctrl_fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0 ||
            ::connect(ctrl_fd_, reinterpret_cast<sockaddr*>(&remote), sizeof remote) != 0) {
            log::warn("wifi: wpa_supplicant control socket %s: %s", ctrl_path_.c_str(), std::strerror(errno));
            close(ctrl_fd_);
            ctrl_fd_ = -1;
            unlink(local_path_.c_str());
            return {};
        }
    }
    char buf[8192];
    while (recv(ctrl_fd_, buf, sizeof buf, MSG_DONTWAIT) > 0) {  // stale replies
    }
    if (send(ctrl_fd_, cmd.data(), cmd.size(), 0) < 0) {
        close(ctrl_fd_);  // wpa_supplicant restarted: connect again next time
        ctrl_fd_ = -1;
        unlink(local_path_.c_str());
        return {};
    }
    double end = mono() + timeout_ms / 1000.0;
    while (true) {
        double left = end - mono();
        if (left <= 0) return {};
        pollfd p{ctrl_fd_, POLLIN, 0};
        if (poll(&p, 1, int(left * 1000) + 1) <= 0) continue;
        ssize_t n = recv(ctrl_fd_, buf, sizeof buf - 1, 0);
        if (n <= 0) return {};
        if (buf[0] == '<') continue;  // an unsolicited event
        return std::string(buf, size_t(n));
    }
}

bool Wifi::rfkill_blocked(bool& found) const {
    found = false;
    bool blocked = false;
    if (DIR* d = opendir("/sys/class/rfkill")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string base = std::string("/sys/class/rfkill/") + e->d_name;
            std::ifstream type(base + "/type");
            std::string t;
            type >> t;
            if (t != "wlan") continue;
            found = true;
            std::ifstream soft(base + "/soft");
            int v = 0;
            soft >> v;
            blocked = blocked || v != 0;
        }
        closedir(d);
    }
    return blocked;
}

bool Wifi::rfkill_set(bool blocked) const {
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    rfkill_event ev{};
    ev.type = RFKILL_TYPE_WLAN;
    ev.op = RFKILL_OP_CHANGE_ALL;
    ev.soft = blocked ? 1 : 0;
    bool ok = write(fd, &ev, sizeof ev) == ssize_t(sizeof ev);
    close(fd);
    return ok;
}

void Wifi::wpa_refresh(bool rescan) {
    if (rescan) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_.scanning = true;
        }
        notify();
        if (wpa("SCAN").rfind("OK", 0) == 0) usleep(3500 * 1000);  // results come in within a few seconds
    }
    std::string status = wpa("STATUS");
    std::string current;
    bool completed = false;
    for (const auto& line : split(status, '\n')) {
        if (line.rfind("ssid=", 0) == 0) current = wpa_unescape(line.substr(5));
        if (line == "wpa_state=COMPLETED") completed = true;
    }
    if (!completed) current.clear();
    // Joined by itself (a saved network after a reboot): Facet's own
    // wpa_supplicant has nobody else to ask for an address.
    if (own_wpa_ && completed && !has_ipv4(state().iface) && mono() - last_dhcp_ > 60) dhcp(state().iface);
    std::vector<std::string> saved;
    for (const auto& line : split(wpa("LIST_NETWORKS"), '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] != "network id") saved.push_back(wpa_unescape(f[1]));
    }
    std::map<std::string, WifiNetwork> by_ssid;
    for (const auto& line : split(wpa("SCAN_RESULTS"), '\n')) {
        auto f = split(line, '\t');  // bssid, frequency, signal level, flags, ssid
        if (f.size() < 5 || f[0] == "bssid / frequency / signal level / flags / ssid") continue;
        std::string ssid = wpa_unescape(f[4]);
        if (ssid.empty() || ssid.find('\0') != std::string::npos) continue;  // hidden
        WifiNetwork n;
        n.ssid = ssid;
        int level = std::atoi(f[2].c_str());
        n.signal = level < 0 ? quality_from_dbm(level) : std::clamp(level, 0, 100);
        const std::string& flags = f[3];
        n.secured = flags.find("WPA") != std::string::npos || flags.find("WEP") != std::string::npos ||
                    flags.find("SAE") != std::string::npos || flags.find("RSN") != std::string::npos;
        n.enterprise = flags.find("EAP") != std::string::npos;
        n.saved = std::find(saved.begin(), saved.end(), ssid) != saved.end();
        n.current = ssid == current;
        auto it = by_ssid.find(ssid);
        if (it == by_ssid.end() || it->second.signal < n.signal) by_ssid[ssid] = n;
    }
    std::vector<WifiNetwork> nets;
    for (auto& [ssid, n] : by_ssid) nets.push_back(n);
    sort_networks(nets);
    bool found = false;
    bool blocked = rfkill_blocked(found);
    std::lock_guard<std::mutex> lock(mu_);
    state_.scanning = false;
    state_.enabled = !blocked;
    state_.networks = std::move(nets);
    state_.current = current;
}

// Adds a network to wpa_supplicant from a saved entry; returns its id or "".
static std::string wpa_add(const std::function<std::string(const std::string&)>& cmd, const Json& n) {
    std::string id = cmd("ADD_NETWORK");
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return {};
    auto set = [&](const std::string& key, const std::string& value) {
        return cmd("SET_NETWORK " + id + " " + key + " " + value).rfind("OK", 0) == 0;
    };
    const std::string& km = n["key_mgmt"].str();
    bool ok = set("ssid", hex(n["ssid"].str()));  // hex: any bytes, no quoting
    if (km == "SAE") {
        ok = ok && set("key_mgmt", "SAE") && set("ieee80211w", "2") && set("sae_password", "\"" + n["sae"].str() + "\"");
    } else if (km == "WPA-PSK") {
        ok = ok && set("key_mgmt", "WPA-PSK") && set("psk", n["psk"].str());
    } else {
        ok = ok && set("key_mgmt", "NONE");
    }
    if (!ok) {
        cmd("REMOVE_NETWORK " + id);
        return {};
    }
    return id;
}

void Wifi::wpa_restore_saved() {
    std::vector<std::string> present;
    for (const auto& line : split(wpa("LIST_NETWORKS"), '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] != "network id") present.push_back(wpa_unescape(f[1]));
    }
    Json saved;
    {
        std::lock_guard<std::mutex> lock(mu_);
        saved = saved_;
    }
    auto cmd = [this](const std::string& c) { return wpa(c); };
    int added = 0;
    for (const auto& n : saved.items()) {
        if (std::find(present.begin(), present.end(), n["ssid"].str()) != present.end()) continue;
        std::string id = wpa_add(cmd, n);
        if (!id.empty()) {
            wpa("ENABLE_NETWORK " + id);
            ++added;
        }
    }
    if (added) {
        log::info("wifi: %d saved network(s) given to wpa_supplicant again", added);
        wpa("SAVE_CONFIG");
        wpa("REASSOCIATE");
    }
}

void Wifi::wpa_connect(const std::string& ssid, const std::string& password) {
    // Is it secured, and how? From the last scan.
    std::string flags;
    for (const auto& line : split(wpa("SCAN_RESULTS"), '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 5 && wpa_unescape(f[4]) == ssid) flags = f[3];
    }
    std::string existing;
    for (const auto& line : split(wpa("LIST_NETWORKS"), '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] != "network id" && wpa_unescape(f[1]) == ssid) existing = f[0];
    }
    auto finish = [&](Phase phase, const std::string& error) {
        if (phase != Phase::Connected)
            log::warn("wifi: connecting to %s failed: %s", ssid.c_str(),
                      phase == Phase::WrongPassword ? "wrong password" : error.c_str());
        else log::info("wifi: connected to %s", ssid.c_str());
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.target == ssid) {
            state_.phase = phase;
            state_.error = error;
        }
    };
    std::string id = existing;
    Json entry = Json::object();
    if (existing.empty() || !password.empty()) {
        bool secured = !flags.empty() && (flags.find("WPA") != std::string::npos ||
                                          flags.find("RSN") != std::string::npos ||
                                          flags.find("SAE") != std::string::npos);
        if (flags.find("WEP") != std::string::npos || flags.find("EAP") != std::string::npos)
            return finish(Phase::Failed, "this kind of network is not supported");
        if (secured && password.empty()) return finish(Phase::WrongPassword, {});
        entry["ssid"] = ssid;
        bool sae_only = flags.find("SAE") != std::string::npos && flags.find("PSK") == std::string::npos;
        if (!secured) entry["key_mgmt"] = "NONE";
        else if (sae_only) {
            entry["key_mgmt"] = "SAE";
            entry["sae"] = password;  // WPA3 needs the password itself
        } else {
            entry["key_mgmt"] = "WPA-PSK";
            entry["psk"] = wpa_psk(ssid, password);
        }
        if (secured && !sae_only && (password.size() < 8 || password.size() > 63))
            return finish(Phase::WrongPassword, {});
        if (!existing.empty()) wpa("REMOVE_NETWORK " + existing);
        auto cmd = [this](const std::string& c) { return wpa(c); };
        id = wpa_add(cmd, entry);
        if (id.empty()) return finish(Phase::Failed, "wpa_supplicant did not take the network");
    }
    wpa("SELECT_NETWORK " + id);
    wpa("RECONNECT");
    Phase phase = Phase::Failed;
    std::string error = "timed out";
    double end = mono() + 30;
    while (mono() < end) {
        usleep(500 * 1000);
        bool completed = false;
        std::string current;
        for (const auto& line : split(wpa("STATUS"), '\n')) {
            if (line == "wpa_state=COMPLETED") completed = true;
            if (line.rfind("ssid=", 0) == 0) current = wpa_unescape(line.substr(5));
        }
        if (completed && current == ssid) {
            phase = Phase::Connected;
            error.clear();
            break;
        }
        // Wrong keys make wpa_supplicant set the network aside for a while.
        bool temp_disabled = false;
        for (const auto& line : split(wpa("LIST_NETWORKS"), '\n')) {
            auto f = split(line, '\t');
            if (f.size() >= 4 && f[0] == id && f[3].find("TEMP-DISABLED") != std::string::npos) temp_disabled = true;
        }
        if (temp_disabled) {
            phase = Phase::WrongPassword;
            error.clear();
            break;
        }
    }
    if (phase == Phase::Connected) {
        if (entry.contains("ssid")) {
            std::lock_guard<std::mutex> lock(mu_);
            Json kept = Json::array();
            for (const auto& n : saved_.items())
                if (n["ssid"].str() != ssid) kept.push_back(n);
            kept.push_back(entry);
            saved_ = kept;
            saved_changed_ = true;
        }
        wpa("SAVE_CONFIG");
        if (own_wpa_) dhcp(state().iface);
    } else {
        if (entry.contains("ssid")) wpa("REMOVE_NETWORK " + id);  // a new network that did not work
        wpa("ENABLE_NETWORK all");  // SELECT_NETWORK switched the others off
        wpa("REASSOCIATE");
    }
    finish(phase, error);
}

}  // namespace facet::net
