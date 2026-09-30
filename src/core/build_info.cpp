#include "core/build_info.h"

#if __has_include("build_info_gen.h")
#include "build_info_gen.h"
#endif

// Fallbacks for builds that did not generate the header.
#ifndef FACET_BUILD_VERSION
#define FACET_BUILD_VERSION "0.0.0-unknown"
#endif
#ifndef FACET_BUILD_CHANNEL
#define FACET_BUILD_CHANNEL "dev"
#endif
#ifndef FACET_BUILD_COMMIT
#define FACET_BUILD_COMMIT ""
#endif
#ifndef FACET_BUILD_DIRTY
#define FACET_BUILD_DIRTY 0
#endif
#ifndef FACET_BUILD_REPO
#define FACET_BUILD_REPO ""
#endif
#ifndef FACET_BUILD_DATE
#define FACET_BUILD_DATE ""
#endif

namespace facet::build {

const Info& info() {
    static const Info i{FACET_BUILD_VERSION, FACET_BUILD_CHANNEL, FACET_BUILD_COMMIT,
                        FACET_BUILD_DIRTY != 0,  FACET_BUILD_REPO,    FACET_BUILD_DATE};
    return i;
}

std::string summary() {
    const Info& i = info();
    std::string s = std::string(i.version) + " · " + i.channel;
    if (*i.commit) s += std::string(" · ") + i.commit + (i.dirty ? "*" : "");
    return s;
}

}  // namespace facet::build
