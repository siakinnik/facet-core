#include "core/netstatus.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace facet::net {

namespace {

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// First line of a command's output, "" on failure. Commands are fixed strings
// built from interface names that come from /sys (no user input).
std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    return out;
}

bool safe_iface(const std::string& s) {
    for (char c : s)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return !s.empty();
}

// Interface of the IPv4 default route, "" if none.
std::string default_route_iface() {
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string iface, dest;
        if (in >> iface >> dest && dest == "00000000") return iface;
    }
    return {};
}

std::string ipv4_of(const std::string& iface) {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof buf);
        out = buf;
        break;
    }
    freeifaddrs(list);
    return out;
}

// Link quality from /proc/net/wireless ("wlan0: 0000   60.  -50. ..."), max 70.
int wifi_signal(const std::string& iface) {
    std::ifstream f("/proc/net/wireless");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string name, status;
        double link = 0;
        if (!(in >> name >> status >> link)) continue;
        if (name == iface + ":") return std::max(0, std::min(100, int(link * 100 / 70)));
    }
    return -1;
}

// SSID straight from the kernel: generic netlink, nl80211 GET_INTERFACE.
// Unprivileged and needs no external tools.
std::string nl80211_ssid(const std::string& iface) {
    unsigned ifindex = if_nametoindex(iface.c_str());
    if (!ifindex) return {};
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return {};
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct Request {
        nlmsghdr n;
        genlmsghdr g;
        char attrs[64];
    };
    auto put_attr = [](Request& r, uint16_t type, const void* data, uint16_t len) {
        auto* a = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(&r) + NLMSG_ALIGN(r.n.nlmsg_len));
        a->nla_type = type;
        a->nla_len = uint16_t(NLA_HDRLEN + len);
        std::memcpy(reinterpret_cast<char*>(a) + NLA_HDRLEN, data, len);
        r.n.nlmsg_len = NLMSG_ALIGN(r.n.nlmsg_len) + NLA_ALIGN(a->nla_len);
    };
    // Calls `fn(type, data, len)` for every attribute of every reply message.
    auto transact = [fd](Request& r, auto&& fn) {
        if (send(fd, &r, r.n.nlmsg_len, 0) < 0) return false;
        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        for (auto* h = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(h, size_t(n)); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_ERROR || h->nlmsg_type == NLMSG_DONE) return false;
            char* p = static_cast<char*>(NLMSG_DATA(h)) + GENL_HDRLEN;
            int left = int(h->nlmsg_len) - int(NLMSG_HDRLEN + GENL_HDRLEN);
            while (left >= int(NLA_HDRLEN)) {
                auto* a = reinterpret_cast<nlattr*>(p);
                if (a->nla_len < NLA_HDRLEN || a->nla_len > left) break;
                fn(a->nla_type & NLA_TYPE_MASK, p + NLA_HDRLEN, int(a->nla_len - NLA_HDRLEN));
                p += NLA_ALIGN(a->nla_len);
                left -= NLA_ALIGN(a->nla_len);
            }
        }
        return true;
    };

    // 1) Resolve the nl80211 family id.
    Request r{};
    r.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    r.n.nlmsg_type = GENL_ID_CTRL;
    r.n.nlmsg_flags = NLM_F_REQUEST;
    r.g.cmd = CTRL_CMD_GETFAMILY;
    r.g.version = 1;
    put_attr(r, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
    uint16_t family = 0;
    transact(r, [&](int type, const char* data, int len) {
        if (type == CTRL_ATTR_FAMILY_ID && len >= 2) std::memcpy(&family, data, 2);
    });

    // 2) Ask for the interface; a connected station reports its SSID.
    std::string ssid;
    if (family) {
        r = Request{};
        r.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
        r.n.nlmsg_type = family;
        r.n.nlmsg_flags = NLM_F_REQUEST;
        r.g.cmd = NL80211_CMD_GET_INTERFACE;
        r.g.version = 0;
        uint32_t idx = ifindex;
        put_attr(r, NL80211_ATTR_IFINDEX, &idx, 4);
        transact(r, [&](int type, const char* data, int len) {
            if (type == NL80211_ATTR_SSID && len > 0 && len <= 32) ssid.assign(data, size_t(len));
        });
    }
    close(fd);
    return ssid;
}

std::string wifi_ssid(const std::string& iface) {
    if (!safe_iface(iface)) return {};
    if (std::string s = nl80211_ssid(iface); !s.empty()) return s;
    // iw: "	SSID: MyNet"
    std::string out = run("iw dev " + iface + " link");
    size_t p = out.find("SSID: ");
    if (p != std::string::npos) return out.substr(p + 6, out.find('\n', p) - p - 6);
    // wireless-tools
    out = run("iwgetid -r " + iface);
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty()) return out;
    // NetworkManager: "yes:MyNet"
    out = run("nmcli -t -f active,ssid dev wifi");
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("yes:", 0) == 0) return line.substr(4);
    return {};
}

bool is_physical(const std::string& iface) { return exists("/sys/class/net/" + iface + "/device"); }
bool is_wifi(const std::string& iface) {
    return exists("/sys/class/net/" + iface + "/wireless") || exists("/sys/class/net/" + iface + "/phy80211");
}
bool is_up(const std::string& iface) {
    std::string state = read_line("/sys/class/net/" + iface + "/operstate");
    return state == "up" || (state == "unknown" && read_line("/sys/class/net/" + iface + "/carrier") == "1");
}

Status probe(std::string& last_ssid_iface, std::string& last_ssid, int& ssid_age) {
    Status s;
    // Prefer the interface that carries the default route; otherwise any
    // physical interface that is up (link without internet).
    std::string iface = default_route_iface();
    if (iface.empty() || !is_up(iface)) {
        iface.clear();
        if (DIR* d = opendir("/sys/class/net")) {
            while (dirent* e = readdir(d)) {
                std::string name = e->d_name;
                if (name[0] == '.' || name == "lo" || !is_physical(name) || !is_up(name)) continue;
                if (iface.empty() || is_wifi(name)) iface = name;
            }
            closedir(d);
        }
    }
    if (iface.empty()) return s;
    s.iface = iface;
    s.ipv4 = ipv4_of(iface);
    if (is_wifi(iface)) {
        s.kind = Kind::Wifi;
        s.signal = wifi_signal(iface);
        // The SSID rarely changes; ask the tools every ~30 s, not every poll.
        if (iface != last_ssid_iface || ++ssid_age >= 6) {
            last_ssid = wifi_ssid(iface);
            last_ssid_iface = iface;
            ssid_age = 0;
        }
        s.ssid = last_ssid;
    } else {
        s.kind = Kind::Ethernet;
    }
    return s;
}

}  // namespace

int bars(int signal) {
    if (signal < 0) return 3;  // unknown: show full rather than alarming
    if (signal >= 67) return 3;
    if (signal >= 34) return 2;
    return signal > 0 ? 1 : 0;
}

Monitor::Monitor() {
    thread_ = std::thread([this] { run(); });
}

Monitor::~Monitor() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

Status Monitor::status() const {
    std::lock_guard<std::mutex> l(mu_);
    return status_;
}

void Monitor::run() {
    std::string ssid_iface, ssid;
    int ssid_age = 0;
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        lock.unlock();
        Status s = probe(ssid_iface, ssid, ssid_age);
        lock.lock();
        if (!(s == status_)) {
            status_ = s;
            changed_ = true;
        }
        cv_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_; });
    }
}

}  // namespace facet::net
