#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace facet::platform {

std::unique_ptr<Platform> make_fbdev();
std::unique_ptr<Platform> make_headless();
std::unique_ptr<Platform> make_gpu(const Options& o, std::unique_ptr<Platform> inner, std::string& error);
#ifdef FACET_HAVE_X11
std::unique_ptr<Platform> make_x11();
#endif

namespace {
std::string g_gpu_failure;
}

const std::string& gpu_failure() { return g_gpu_failure; }

std::unique_ptr<Platform> create_platform(const Options& options) {
    const char* want = std::getenv("FACET_BACKEND");
    bool gpu = want ? std::strcmp(want, "gpu") == 0 : options.gpu;
    // After repeated helper crashes the CPU is used until the user tries again.
    if (gpu && !options.data_dir.empty()) {
        if (FILE* f = std::fopen((options.data_dir + "/gl/failed").c_str(), "r")) {
            char buf[256] = {};
            size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            g_gpu_failure = std::string(buf, n);
            gpu = false;
        }
    }
    if (gpu) {
        // FACET_GPU_OFFSCREEN=1 (tests): the helper renders without a screen;
        // input and size come from the headless backend.
        std::unique_ptr<Platform> inner;
        if (std::getenv("FACET_GPU_OFFSCREEN")) inner = make_headless();
        std::string error;
        if (auto p = make_gpu(options, std::move(inner), error)) return p;
        g_gpu_failure = error;
        log::warn("platform: GPU unavailable (%s); using the CPU", error.c_str());
        if (want && std::getenv("FACET_GPU_OFFSCREEN")) return make_headless();
    }
    if (want && std::strcmp(want, "headless") == 0) return make_headless();
#ifdef FACET_HAVE_X11
    if ((want && std::strcmp(want, "x11") == 0) || (!want && std::getenv("DISPLAY"))) return make_x11();
#endif
    return make_fbdev();
}

}  // namespace facet::platform
