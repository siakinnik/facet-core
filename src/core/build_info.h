// What this binary is: version, build channel, git commit and repository.
// Filled in at build time (cmake/build_info.cmake or scripts/install.sh).
#pragma once

#include <string>

namespace facet::build {

struct Info {
    const char* version;  // "0.0.1-alpha"
    const char* channel;  // "dev" or "release"
    const char* commit;   // short hash, "" if unknown
    bool dirty;           // built with uncommitted changes
    const char* repo;     // "github.com/owner/name", "" if unknown
    const char* date;     // build date, UTC
};

const Info& info();

// "0.0.1-alpha · dev · 3f2a1b9c0d1e*" (the star marks uncommitted changes).
std::string summary();

}  // namespace facet::build
