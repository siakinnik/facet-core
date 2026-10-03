// The graphics devices of the machine and the downloadable OpenGL package
// (Mesa drivers and the facet-gpu helper) that lets Facet draw with the GPU.
// The package is built with every core release and published next to it;
// release builds know its SHA-256, so a download is accepted only if it is
// exactly that file. Downloading uses the system's curl (or wget), unpacking
// its tar, both of which the installer script needs anyway.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace facet::gpu {

struct GpuInfo {
    std::string card;    // /dev/dri/card0
    std::string driver;  // kernel driver: i915, amdgpu, vc4, ...
    std::string vendor;  // "Intel", "AMD", "NVIDIA", "Broadcom", ...
    std::string model;   // "UHD Graphics 620" from the system's PCI database, "" if unknown
    bool primary = false;    // the firmware's boot display device
    bool supported = false;  // drawn by the drivers in the package
    std::string name() const;  // "Intel UHD Graphics 620", or "Intel (i915)" without a model
};

// Display-capable devices under /sys/class/drm, the primary one first.
std::vector<GpuInfo> detect_gpus();
// The render node of a card ("/dev/dri/card1" -> "/dev/dri/renderD128"), "" if none.
std::string render_node(const std::string& card);

class GlPackage {
public:
    enum class Phase { Idle, Downloading, Verifying, Unpacking, Done, Failed };
    struct Status {
        Phase phase = Phase::Idle;
        double progress = 0;
        std::string error;  // English, when Failed
    };

    explicit GlPackage(std::string root);  // <data>/gl
    ~GlPackage();

    // A package for this build exists (release builds; FACET_GL_URL for tests).
    bool available() const;
    uint64_t size() const;           // download size in bytes, 0 if unknown
    std::string installed() const;   // version of the installed package, "" if none
    std::string dir() const;         // <root>/current
    bool busy() const { return running_; }
    Status status() const;
    void acknowledge();  // Done/Failed -> Idle
    void install();
    void cancel() { cancel_ = true; }
    void remove();

private:
    void run();
    void set(Phase p, double progress = 0);

    std::string root_, url_, sha256_;
    uint64_t size_ = 0;
    mutable std::mutex mu_;
    Status status_;
    std::thread worker_;
    std::atomic<bool> running_{false}, cancel_{false};
};

}  // namespace facet::gpu
