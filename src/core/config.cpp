#include "core/config.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>

#include "core/log.h"

namespace facet {

namespace paths {

bool mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

std::string exe_dir() {
    char exe[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return ".";
    std::string p(exe, size_t(n));
    return p.substr(0, p.rfind('/'));
}

const std::string& data_root() {
    static const std::string root = [] {
        if (const char* d = std::getenv("FACET_DATA")) return std::string(d);
        if (mkdirs("/var/lib/facet") && ::access("/var/lib/facet", W_OK) == 0) return std::string("/var/lib/facet");
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.local/share/facet";
    }();
    return root;
}

std::vector<std::string> plugin_dirs() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("FACET_PLUGIN_PATH")) {
        std::string s = env;
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(':', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) dirs.push_back(s.substr(start, end - start));
            start = end + 1;
        }
    }
    std::string bin = exe_dir();
    dirs.push_back(bin + "/plugins");
    dirs.push_back(bin + "/../lib/facet/plugins");  // <prefix>/bin/facet -> <prefix>/lib/facet/plugins
    dirs.push_back("/usr/local/lib/facet/plugins");
    dirs.push_back("/usr/lib/facet/plugins");
    dirs.push_back(data_root() + "/plugins");
    return dirs;
}

}  // namespace paths

void Config::load(const std::string& path) {
    path_ = path;
    Json j;
    if (load_json_file(path, j) && j.is_object()) root_ = std::move(j);
    else log::info("config: %s not found, using defaults", path.c_str());
}

void Config::set(const std::string& key, Json value) {
    if (root_[key] == value) return;
    root_[key] = std::move(value);
    if (!dirty_) dirty_since_ = -1;  // stamped by the next save_if_dirty call
    dirty_ = true;
}

void Config::save_if_dirty(double now, bool force) {
    if (!dirty_) return;
    if (dirty_since_ < 0) dirty_since_ = now;
    if (!force && now - dirty_since_ < 1.0) return;
    if (!save_json_file(path_, root_)) log::warn("config: cannot write %s", path_.c_str());
    dirty_ = false;
}

}  // namespace facet
