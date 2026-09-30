#include "core/timezone.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "core/log.h"

namespace facet::tz {

namespace {

std::string zoneinfo_dir() {
    const char* d = std::getenv("TZDIR");
    return d && *d ? d : "/usr/share/zoneinfo";
}

bool read_tab(const std::string& path, std::vector<std::string>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string codes, coords, zone;
        if (std::getline(in, codes, '\t') && std::getline(in, coords, '\t') && std::getline(in, zone, '\t'))
            out.push_back(zone);
    }
    return !out.empty();
}

}  // namespace

const std::vector<std::string>& zones() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v;
        std::string dir = zoneinfo_dir();
        if (!read_tab(dir + "/zone1970.tab", v)) read_tab(dir + "/zone.tab", v);
        v.push_back("UTC");
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    return list;
}

std::string system_zone() {
    // /etc/localtime -> .../zoneinfo/Europe/Berlin
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/etc/localtime", buf, sizeof buf - 1);
    if (n > 0) {
        std::string target(buf, size_t(n));
        size_t pos = target.find("zoneinfo/");
        if (pos != std::string::npos) return target.substr(pos + 9);
    }
    std::ifstream f("/etc/timezone");  // Debian-style fallback
    std::string zone;
    if (f >> zone) return zone;
    return {};
}

bool apply(const std::string& zone) {
    if (zone.empty()) {
        unsetenv("TZ");
        tzset();
        return true;
    }
    struct stat st;
    if (zone != "UTC" && ::stat((zoneinfo_dir() + "/" + zone).c_str(), &st) != 0) {
        log::warn("timezone: unknown zone '%s', using the system zone", zone.c_str());
        unsetenv("TZ");
        tzset();
        return false;
    }
    setenv("TZ", zone == "UTC" ? "UTC0" : (":" + zone).c_str(), 1);
    tzset();
    return true;
}

std::string region_of(const std::string& zone) {
    size_t slash = zone.find('/');
    return slash == std::string::npos ? zone : zone.substr(0, slash);
}

std::string city_label(const std::string& zone) {
    size_t slash = zone.find('/');
    std::string city = slash == std::string::npos ? zone : zone.substr(slash + 1);
    std::replace(city.begin(), city.end(), '_', ' ');
    return city;
}

}  // namespace facet::tz
