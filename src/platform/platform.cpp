#include "platform/platform.h"

#include <cstdlib>
#include <cstring>

namespace facet::platform {

std::unique_ptr<Platform> make_fbdev();
std::unique_ptr<Platform> make_headless();
#ifdef FACET_HAVE_X11
std::unique_ptr<Platform> make_x11();
#endif

std::unique_ptr<Platform> create_platform() {
    const char* want = std::getenv("FACET_BACKEND");
    if (want && std::strcmp(want, "headless") == 0) return make_headless();
#ifdef FACET_HAVE_X11
    if ((want && std::strcmp(want, "x11") == 0) || (!want && std::getenv("DISPLAY"))) return make_x11();
#endif
    (void)want;
    return make_fbdev();
}

}  // namespace facet::platform
