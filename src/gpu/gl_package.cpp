#include "gpu/gl_package.h"

#include <dirent.h>
#include <ftw.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>

#include "core/build_info.h"
#include "core/log.h"
#include "gpu/sha256.h"

extern char** environ;

#ifndef FACET_GL_SHA256
#define FACET_GL_SHA256 ""  // set for release builds by CMake
#endif
#ifndef FACET_GL_SIZE
#define FACET_GL_SIZE 0
#endif

namespace facet::gpu {

namespace {

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string link_name(const std::string& path) {
    char buf[4096];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return {};
    std::string s(buf, size_t(n));
    return s.substr(s.rfind('/') + 1);
}

int remove_entry(const char* path, const struct stat*, int, struct FTW*) {
    ::remove(path);
    return 0;
}

void remove_tree(const std::string& path) { nftw(path.c_str(), remove_entry, 32, FTW_DEPTH | FTW_PHYS); }

const char* arch() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "armv7";
#else
    return "unknown";
#endif
}

// Drivers the package's Mesa can draw with.
bool mesa_supports(const std::string& driver) {
    static const char* known[] = {"i915", "xe", "amdgpu", "radeon", "nouveau", "virtio_gpu", "vmwgfx",
                                  "vc4", "v3d", "panfrost", "lima", "msm", "etnaviv"};
    for (const char* k : known)
        if (driver == k) return true;
    return false;
}

std::string vendor_of(const std::string& pci_id, const std::string& driver) {
    if (pci_id == "0x8086") return "Intel";
    if (pci_id == "0x1002") return "AMD";
    if (pci_id == "0x10de") return "NVIDIA";
    if (pci_id == "0x1af4") return "Virtio";
    if (pci_id == "0x15ad") return "VMware";
    if (driver == "vc4" || driver == "v3d") return "Broadcom";
    if (driver == "panfrost" || driver == "lima") return "Arm Mali";
    if (driver == "msm") return "Qualcomm Adreno";
    if (driver == "etnaviv") return "Vivante";
    return driver;
}

// Runs a command and waits, calling `tick` every 200 ms (false: kill it).
int run_command(const std::vector<std::string>& args, const std::function<bool()>& tick) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0) return -1;
        if (tick && !tick()) {
            ::kill(pid, SIGTERM);
            waitpid(pid, &st, 0);
            return -2;
        }
        usleep(200000);
    }
}

bool have(const char* tool) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        if (::access((p.substr(start, end - start) + "/" + tool).c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

}  // namespace

std::string GpuInfo::name() const {
    if (!model.empty()) return vendor == driver ? model : vendor + " " + model;
    return vendor == driver ? driver : vendor + " (" + driver + ")";
}

namespace {
// The device's name in pci.ids (pciutils / hwdata): "Kaby Lake-R GT2 [UHD
// Graphics 620]" gives the marketing name in brackets.
std::string pci_model(const std::string& vendor_id, const std::string& device_id) {
    auto hex = [](std::string v) {
        if (v.compare(0, 2, "0x") == 0) v = v.substr(2);
        for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::string ven = hex(vendor_id), dev = hex(device_id);
    if (ven.size() != 4 || dev.size() != 4) return {};
    for (const char* path : {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids"}) {
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        bool in_vendor = false;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] != '	') {
                if (in_vendor) break;
                in_vendor = line.compare(0, 4, ven) == 0 && line.size() > 4 && line[4] == ' ';
                continue;
            }
            if (!in_vendor || line.size() < 7 || line[1] == '	' || line.compare(1, 4, dev) != 0) continue;
            std::string name = line.substr(5);
            name.erase(0, name.find_first_not_of(' '));
            size_t open = name.find('['), close = name.rfind(']');
            if (open != std::string::npos && close != std::string::npos && close > open + 1)
                name = name.substr(open + 1, close - open - 1);
            return name;
        }
        return {};
    }
    return {};
}
}  // namespace

std::string render_node(const std::string& card) {
    size_t slash = card.rfind('/');
    if (slash == std::string::npos) return {};
    std::string dir = "/sys/class/drm/" + card.substr(slash + 1) + "/device/drm";
    DIR* d = opendir(dir.c_str());
    if (!d) return {};
    std::string node;
    while (dirent* e = readdir(d))
        if (std::strncmp(e->d_name, "renderD", 7) == 0) node = std::string("/dev/dri/") + e->d_name;
    closedir(d);
    return node;
}

std::vector<GpuInfo> detect_gpus() {
    std::vector<GpuInfo> out;
    DIR* d = opendir("/sys/class/drm");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        // card0, card1, ...; not the connectors (card0-eDP-1).
        if (n.compare(0, 4, "card") != 0 || n.size() == 4 || n.find('-') != std::string::npos) continue;
        std::string sys = "/sys/class/drm/" + n + "/device";
        GpuInfo g;
        g.card = "/dev/dri/" + n;
        g.driver = link_name(sys + "/driver");
        // Firmware framebuffers (simpledrm, efifb) are not GPUs.
        if (g.driver.empty() || g.driver == "simpledrm" || g.driver == "simple-framebuffer") continue;
        g.vendor = vendor_of(read_text(sys + "/vendor"), g.driver);
        g.model = pci_model(read_text(sys + "/vendor"), read_text(sys + "/device"));
        g.primary = read_text(sys + "/boot_vga") == "1";
        g.supported = mesa_supports(g.driver);
        out.push_back(g);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const GpuInfo& a, const GpuInfo& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.supported != b.supported) return a.supported;
        return a.card < b.card;
    });
    return out;
}

GlPackage::GlPackage(std::string root) : root_(std::move(root)) {
    const std::string version = build::info().version;
    url_ = "https://github.com/siakinnik/facet-core/releases/download/v" + version + "/facet-gl-" + version +
           "-linux-" + arch() + ".tar.gz";
    sha256_ = FACET_GL_SHA256;
    size_ = uint64_t(FACET_GL_SIZE);
    // Tests and development: another package.
    if (const char* u = std::getenv("FACET_GL_URL")) url_ = u;
    if (const char* s = std::getenv("FACET_GL_SHA256")) sha256_ = s;
    if (const char* z = std::getenv("FACET_GL_SIZE")) size_ = uint64_t(std::strtoull(z, nullptr, 10));
}

GlPackage::~GlPackage() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool GlPackage::available() const { return sha256_.size() == 64; }
uint64_t GlPackage::size() const { return size_; }
std::string GlPackage::dir() const { return root_ + "/current"; }

std::string GlPackage::installed() const {
    struct stat st;
    if (::stat((dir() + "/bin/facet-gpu").c_str(), &st) != 0) return {};
    std::string v = read_text(dir() + "/VERSION");
    return v.empty() ? "?" : v;
}

GlPackage::Status GlPackage::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void GlPackage::acknowledge() {
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.phase == Phase::Done || status_.phase == Phase::Failed) status_.phase = Phase::Idle;
}

void GlPackage::set(Phase p, double progress) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = p;
    status_.progress = progress;
}

void GlPackage::install() {
    if (running_ || !available()) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    running_ = true;
    set(Phase::Downloading);
    worker_ = std::thread([this] {
        run();
        running_ = false;
    });
}

void GlPackage::remove() {
    if (running_) return;
    remove_tree(root_);
    remove_tree(root_ + "-cache");  // <data>/gl-cache: the helper's shader cache
}

void GlPackage::run() {
    auto fail = [this](const std::string& e) {
        log::warn("gl: %s", e.c_str());
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Failed;
        status_.error = e;
    };
    ::mkdir(root_.c_str(), 0755);
    std::string tmp = root_ + "/download.tar.gz";
    ::remove(tmp.c_str());
    log::info("gl: downloading %s", url_.c_str());
    std::vector<std::string> cmd;
    if (have("curl")) cmd = {"curl", "-fsSL", "--retry", "3", "-o", tmp, url_};
    else if (have("wget")) cmd = {"wget", "-q", "-O", tmp, url_};
    else return fail("neither curl nor wget is installed");
    int rc = run_command(cmd, [&] {
        struct stat st;
        if (size_ && ::stat(tmp.c_str(), &st) == 0) {
            std::lock_guard<std::mutex> lock(mu_);
            status_.progress = std::min(1.0, double(st.st_size) / double(size_));
        }
        return !cancel_.load();
    });
    if (rc == -2) return fail("cancelled");
    if (rc != 0) return fail("the download failed (no network?)");

    set(Phase::Verifying);
    if (sha256_file(tmp) != sha256_) {
        ::remove(tmp.c_str());
        return fail("the download is damaged (checksum mismatch)");
    }

    set(Phase::Unpacking);
    std::string version = build::info().version;
    std::string target = root_ + "/" + version, fresh = target + ".new";
    remove_tree(fresh);
    ::mkdir(fresh.c_str(), 0755);
    if (run_command({"tar", "-xzf", tmp, "-C", fresh, "--strip-components=1", "--no-same-owner"}, nullptr) != 0) {
        remove_tree(fresh);
        return fail("cannot unpack the package");
    }
    struct stat st;
    if (::stat((fresh + "/bin/facet-gpu").c_str(), &st) != 0) {
        remove_tree(fresh);
        return fail("the package has no GPU helper");
    }
    remove_tree(target);
    if (::rename(fresh.c_str(), target.c_str()) != 0) return fail("cannot install the package");
    std::string link = root_ + "/current.new";
    ::unlink(link.c_str());
    if (::symlink(version.c_str(), link.c_str()) != 0 || ::rename(link.c_str(), dir().c_str()) != 0)
        return fail("cannot activate the package");
    // Older versions and an old "the GPU failed" mark go.
    if (DIR* d = opendir(root_.c_str())) {
        std::vector<std::string> old;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != ".." && n != "current" && n != version && n != "gl-cache") old.push_back(root_ + "/" + n);
        }
        closedir(d);
        for (const auto& o : old) remove_tree(o);
    }
    log::info("gl: installed the OpenGL package %s", version.c_str());
    set(Phase::Done, 1);
}

}  // namespace facet::gpu
