#include "platform/console_log.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace facet::platform {

namespace {

bool read_levels(const std::string& path, int v[4]) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    int n = std::fscanf(f, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
    std::fclose(f);
    return n == 4;
}

bool write_levels(const std::string& path, const int v[4]) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    bool ok = std::fprintf(f, "%d %d %d %d\n", v[0], v[1], v[2], v[3]) > 0;
    return std::fclose(f) == 0 && ok;
}

}  // namespace

bool ConsoleLog::quiet(int level) {
    if (active_) return true;
    int cur[4];
    if (!read_levels(printk_, cur)) return false;
    // A run of ours that did not get to restore left the level it found here.
    int saved[4];
    bool have_saved = read_levels(saved_, saved);
    if (!have_saved && cur[0] <= level) return false;  // already as quiet as asked
    if (!have_saved) {
        size_t slash = saved_.rfind('/');
        if (slash != std::string::npos && slash > 0) ::mkdir(saved_.substr(0, slash).c_str(), 0755);
        if (!write_levels(saved_, cur)) return false;
        std::memcpy(saved, cur, sizeof saved);
    }
    int next[4] = {std::min(cur[0], level), cur[1], cur[2], cur[3]};
    if (!write_levels(printk_, next)) {  // not root
        if (!have_saved) ::unlink(saved_.c_str());
        return false;
    }
    original_ = saved[0];
    level_ = next[0];
    active_ = true;
    log::info("facet: the kernel's messages on the console are limited to level %d while Facet runs (was %d)",
              level_, original_);
    return true;
}

void ConsoleLog::restore() {
    if (!active_) return;
    active_ = false;
    int cur[4];
    // Someone else changed it meanwhile: theirs stays.
    if (read_levels(printk_, cur) && cur[0] == level_) {
        cur[0] = original_;
        write_levels(printk_, cur);
    }
    ::unlink(saved_.c_str());
}

}  // namespace facet::platform
