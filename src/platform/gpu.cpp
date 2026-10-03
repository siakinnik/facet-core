// GPU backend: the core keeps drawing its UI on the CPU, but a helper
// process (facet-gpu, from the downloadable OpenGL package) owns the screen
// through DRM/KMS and composes the canvas with the plugins' surfaces on the
// GPU. Surfaces are not copied by the core any more: the helper maps their
// buffer files and draws them under the canvas' see-through parts; when only
// a surface changed, the canvas is not redrawn at all.
//
// The helper is a separate process because the core is a static binary and
// cannot load the GPU drivers. If it fails, Facet falls back to fbdev.
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <thread>

#include "core/log.h"
#include "gpu/channel.h"
#include "platform/backlight.h"
#include "platform/evdev_touch.h"
#include "platform/platform.h"

extern char** environ;

namespace facet::platform {

namespace {
double mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

class GpuPlatform final : public Platform {
public:
    GpuPlatform(Options o, std::unique_ptr<Platform> inner) : opt_(std::move(o)), inner_(std::move(inner)) {}

    ~GpuPlatform() override {
        if (ch_.valid()) {
            Json q = Json::object();
            q["t"] = "quit";
            ch_.send(q);
        }
        stop_helper();
        if (canvas_) munmap(canvas_, canvas_size_);
        if (mfd_ >= 0) ::close(mfd_);
        if (ops_map_) munmap(ops_map_, kOpsCapacity);
        if (ops_fd_ >= 0) ::close(ops_fd_);
    }

    const char* name() const override { return "gpu"; }

    // Starts the helper; called by create_platform() so that a failure can
    // fall back to fbdev. A second call is a no-op.
    bool init() override {
        if (ready_) return true;
        if (inner_ && !inner_->init()) return fail("the input backend did not start");
        if (!start_helper()) return false;
        if (!inner_) {
            backlight_.open();
            touch_.open_devices(w_, h_);
        }
        ready_ = true;
        return true;
    }

    const std::string& error() const { return error_; }

    int width() const override { return w_; }
    int height() const override { return h_; }

    void add_poll_fds(std::vector<pollfd>& fds) override {
        if (inner_) inner_->add_poll_fds(fds);
        else touch_.add_poll_fds(fds);
        if (ch_.valid()) fds.push_back({ch_.fd(), POLLIN, 0});
    }

    void pump(std::vector<Event>& out) override {
        if (inner_) inner_->pump(out);
        else touch_.pump(out, mono());
        read_helper();
    }

    void present(const gfx::Canvas& canvas) override { present_layers(canvas, true, {}); }

    bool supports_layers() const override { return true; }
    uint32_t* canvas_memory() override { return static_cast<uint32_t*>(canvas_); }
    bool ready_to_draw() const override { return !canvas_busy_; }

    void wait_ready(int ms) override {
        double end = mono() + ms / 1000.0;
        while (canvas_busy_ && ch_.valid()) {
            int left = int((end - mono()) * 1000);
            if (left <= 0) break;
            pollfd p{ch_.fd(), POLLIN, 0};
            if (::poll(&p, 1, left) <= 0) break;
            read_helper();
        }
    }

    bool supports_ops() const override { return ops_; }
    bool supports_gpu_buffers() const override { return ops_ && dmabuf_; }
    bool take_ops_lost() override {
        bool lost = ops_lost_;
        ops_lost_ = false;
        return lost;
    }

    void present_ops(const std::vector<uint8_t>& ops, const std::vector<Layer>& layers) override {
        if (!ops_map_) return;
        size_t n = ops.size();
        if (n > kOpsCapacity) {
            log::warn("gpu: a frame of %zu bytes of drawing commands is too big", n);
            n = 0;
        }
        if (n) std::memcpy(ops_map_, ops.data(), n);
        send_frame_of(layers, n > 0, n);
    }

    void present_layers(const gfx::Canvas& canvas, bool canvas_changed, const std::vector<Layer>& layers) override {
        (void)canvas;
        send_frame_of(layers, canvas_changed, 0);
    }

private:
    // `changed`: the canvas (in ops mode: the commands, `ops_bytes` of them) is new.
    void send_frame_of(const std::vector<Layer>& layers, bool changed, size_t ops_bytes) {
        if (!ch_.valid()) return;
        const bool canvas_changed = changed;
        // Surfaces the helper does not know yet get their buffer file; the
        // ones no longer on screen are dropped.
        std::set<std::string> now;
        for (const auto& l : layers) {
            now.insert(l.key);
            if (known_.count(l.key)) continue;
            int fd = ::open(l.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) continue;
            Json m = Json::object();
            m["t"] = "surface";
            m["key"] = l.key;
            m["w"] = l.w;
            m["h"] = l.h;
            m["stride"] = l.stride;
            m["buffers"] = l.buffers;
            ch_.send(m, fd);
            ::close(fd);
            known_.insert(l.key);
        }
        for (auto it = known_.begin(); it != known_.end();) {
            if (now.count(*it)) {
                ++it;
                continue;
            }
            Json m = Json::object();
            m["t"] = "surface_drop";
            m["key"] = *it;
            ch_.send(m);
            for (auto g = known_gpu_.begin(); g != known_gpu_.end();)
                g = g->rfind(*it + "|", 0) == 0 ? known_gpu_.erase(g) : std::next(g);
            it = known_.erase(it);
        }
        // GPU buffers the helper has not imported yet (a new slot or attach).
        for (const auto& l : layers) {
            if (l.gpu < 0 || l.gpu_fd < 0 || !known_.count(l.key)) continue;
            std::string gk = l.key + "|" + std::to_string(l.gpu) + "|" + std::to_string(l.gpu_generation);
            if (known_gpu_.count(gk)) continue;
            Json m = Json::object();
            m["t"] = "surface_gpu";
            m["key"] = l.key;
            m["slot"] = l.gpu;
            m["w"] = l.gpu_w;
            m["h"] = l.gpu_h;
            m["format"] = double(l.gpu_format);
            m["mod_hi"] = double(uint32_t(l.gpu_modifier >> 32));
            m["mod_lo"] = double(uint32_t(l.gpu_modifier));
            m["offset"] = double(l.gpu_offset);
            m["stride"] = double(l.gpu_stride);
            ch_.send(m, l.gpu_fd);
            for (auto g = known_gpu_.begin(); g != known_gpu_.end();)  // the slot's older buffer
                g = g->rfind(l.key + "|" + std::to_string(l.gpu) + "|", 0) == 0 ? known_gpu_.erase(g) : std::next(g);
            known_gpu_.insert(gk);
        }
        Json frame = Json::object();
        frame["t"] = "frame";
        Json damage = Json::array();
        bool upload = canvas_changed || (pending_ && pending_canvas_);
        if (ops_) {
            // New commands, or the ones a frame still waiting for the screen carries.
            if (canvas_changed) {
                frame["ops"] = Json(double(ops_bytes));
                canvas_busy_ = true;  // until the helper has read them
            } else if (pending_ && pending_canvas_) {
                frame["ops"] = pending_frame_["ops"];
            }
        } else if (upload) {
            Json r = Json::array();
            for (int v : {0, 0, w_, h_}) r.push_back(v);
            damage.push_back(r);
            canvas_busy_ = true;  // until the helper has uploaded it
        }
        frame["canvas"] = damage;
        Json ls = Json::array();
        std::vector<std::string> keys;
        for (const auto& l : layers) {
            Json j = Json::object();
            j["key"] = l.key;
            j["buffer"] = l.buffer;
            j["dst"] = rect(l.dst);
            j["clip"] = rect(l.clip);
            if (l.gpu >= 0) j["gpu"] = l.gpu;
            ls.push_back(j);
            // Handed back at "uploaded" (a GPU buffer too: the helper has finished
            // every draw that read the buffer it replaces, and draws this frame from it).
            keys.push_back(l.key);
        }
        frame["layers"] = ls;
        if (in_flight_) {  // one frame at a time: the newest waits for the screen
            pending_ = true;
            pending_canvas_ = upload;
            pending_frame_ = frame;
            pending_keys_ = keys;
            return;
        }
        send_frame(frame, keys);
    }

public:
    std::vector<std::string> take_uploaded() override {
        std::vector<std::string> out;
        out.swap(uploaded_);
        return out;
    }

    void set_display_power(bool on) override {
        if (ch_.valid()) {
            Json m = Json::object();
            m["t"] = "power";
            m["on"] = on;
            ch_.send(m);
        }
        if (inner_) inner_->set_display_power(on);
        else backlight_.set_power(on);
    }

    bool has_brightness() const override { return inner_ ? inner_->has_brightness() : backlight_.available(); }
    void set_brightness(int percent) override {
        if (inner_) inner_->set_brightness(percent);
        else backlight_.set_percent(percent);
    }

private:
    static Json rect(const gfx::Rect& r) {
        Json a = Json::array();
        for (float v : {r.x, r.y, r.w, r.h}) a.push_back(int(std::lround(v)));
        return a;
    }

    bool fail(const std::string& e) {
        error_ = e;
        log::warn("gpu: %s", e.c_str());
        return false;
    }

    bool start_helper() {
        const char* dev = std::getenv("FACET_GPU_HELPER");  // development: a helper using the system's drivers
        std::string helper = dev ? dev : opt_.gl_dir + "/bin/facet-gpu";
        if (!exists(helper)) return fail("the OpenGL package is not installed");
        bool offscreen = inner_ != nullptr;
        if (!offscreen && (opt_.card.empty() || !exists(opt_.card))) return fail("no graphics device " + opt_.card);

        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return fail("socketpair failed");
        bool helper_ops = false;
        std::vector<std::string> args = {helper, "--fd", "3"};
        if (offscreen) {
            args.push_back("--offscreen");
            args.push_back(std::to_string(inner_->width()) + "x" + std::to_string(inner_->height()));
            if (const char* dev = std::getenv("FACET_GPU_DEVICE")) {  // tests on a real GPU (a render node)
                args.push_back("--device");
                args.push_back(dev);
            }
            if (const char* dump = std::getenv("FACET_GPU_DUMP")) {
                args.push_back("--dump");
                args.push_back(dump);
            }
        } else {
            args.push_back("--card");
            args.push_back(opt_.card);
        }
        std::vector<std::string> env;
        for (char** e = environ; *e; ++e) env.emplace_back(*e);
        if (!dev) {  // the package's own Mesa
            const std::string& g = opt_.gl_dir;
            env.push_back("LD_LIBRARY_PATH=" + g + "/lib");
            env.push_back("LIBGL_DRIVERS_PATH=" + g + "/lib/dri");
            env.push_back("GBM_BACKENDS_PATH=" + g + "/lib/gbm");
            env.push_back("__EGL_VENDOR_LIBRARY_FILENAMES=" + g + "/share/glvnd/egl_vendor.d/50_mesa.json");
        }
        if (!opt_.data_dir.empty()) env.push_back("MESA_SHADER_CACHE_DIR=" + opt_.data_dir + "/gl-cache");
        std::vector<char*> a, e;
        for (auto& s : args) a.push_back(s.data());
        a.push_back(nullptr);
        for (auto& s : env) e.push_back(s.data());
        e.push_back(nullptr);
        pid_ = fork();
        if (pid_ == 0) {
            dup2(sv[1], 3);
            for (int fd = 4; fd < 1024; ++fd) ::close(fd);
            execve(a[0], a.data(), e.data());
            _exit(127);
        }
        ::close(sv[1]);
        if (pid_ < 0) {
            ::close(sv[0]);
            return fail("fork failed");
        }
        ch_.adopt(sv[0], false);
        // Wait for "ready" (drivers load, the display is taken).
        double deadline = mono() + 15;
        while (mono() < deadline) {
            std::vector<std::pair<Json, int>> msgs;
            if (!ch_.read(msgs, 500)) break;
            for (auto& [m, f] : msgs) {
                if (f >= 0) ::close(f);
                if (m["t"].str() == "error") {
                    stop_helper();
                    return fail(m["message"].str());
                }
                if (m["t"].str() == "ready") {
                    w_ = m["w"].as_int();
                    h_ = m["h"].as_int();
                    renderer_ = m["renderer"].str();
                    // Newer helpers draw the interface themselves; FACET_GPU_OPS=0 keeps the canvas.
                    const char* env = std::getenv("FACET_GPU_OPS");
                    helper_ops = m["ops"].as_bool(false) && !(env && std::string(env) == "0");
                    dmabuf_ = m["dmabuf"].as_bool(false);
                }
            }
            if (w_ > 0) break;
        }
        if (w_ <= 0 || h_ <= 0) {
            stop_helper();
            return fail("the GPU helper did not start");
        }
        int fl = fcntl(ch_.fd(), F_GETFL);
        fcntl(ch_.fd(), F_SETFL, fl | O_NONBLOCK);
        if (ready_ && helper_ops != ops_) {  // a restarted helper must draw the same way
            stop_helper();
            return fail("the GPU helper changed");
        }
        ops_ = helper_ops;
        if (ops_) {
            // Commands go through shared memory; the file is sparse, so its
            // size is only an upper bound.
            if (!ops_map_) {
                ops_fd_ = memfd_create("facet-ops", MFD_CLOEXEC);
                if (ops_fd_ < 0 || ftruncate(ops_fd_, off_t(kOpsCapacity)) != 0) {
                    stop_helper();
                    return fail("no shared memory for drawing commands");
                }
                void* m = mmap(nullptr, kOpsCapacity, PROT_READ | PROT_WRITE, MAP_SHARED, ops_fd_, 0);
                if (m == MAP_FAILED) {
                    stop_helper();
                    return fail("cannot map the drawing commands");
                }
                ops_map_ = static_cast<uint8_t*>(m);
            }
            Json o = Json::object();
            o["t"] = "ops";
            o["size"] = Json(double(kOpsCapacity));
            ch_.send(o, ops_fd_);
            ops_lost_ = true;
            log::info("gpu: %s, %dx%d (%s), draws the interface", renderer_.c_str(), w_, h_,
                      offscreen ? "offscreen" : opt_.card.c_str());
            return true;
        }

        // The canvas memory outlives helper restarts: the core keeps drawing into it.
        if (!canvas_) {
            canvas_size_ = size_t(w_) * size_t(h_) * 4;
            mfd_ = memfd_create("facet-canvas", MFD_CLOEXEC);
            if (mfd_ < 0 || ftruncate(mfd_, off_t(canvas_size_)) != 0) {
                stop_helper();
                return fail("no shared memory for the canvas");
            }
            canvas_ = mmap(nullptr, canvas_size_, PROT_READ | PROT_WRITE, MAP_SHARED, mfd_, 0);
            if (canvas_ == MAP_FAILED) {
                canvas_ = nullptr;
                stop_helper();
                return fail("cannot map the canvas");
            }
        } else if (canvas_size_ != size_t(w_) * size_t(h_) * 4) {
            stop_helper();
            return fail("the display mode changed");
        }
        Json c = Json::object();
        c["t"] = "canvas";
        c["w"] = w_;
        c["h"] = h_;
        ch_.send(c, mfd_);
        log::info("gpu: %s, %dx%d (%s)", renderer_.c_str(), w_, h_, offscreen ? "offscreen" : opt_.card.c_str());
        return true;
    }

    void stop_helper() {
        ch_.close();
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            for (int i = 0; i < 30 && waitpid(pid_, nullptr, WNOHANG) == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }

    void send_frame(Json frame, const std::vector<std::string>& keys) {
        int seq = ++seq_;
        frame["seq"] = seq;
        seq_keys_[seq] = keys;
        in_flight_ = true;
        if (!ch_.send(frame)) helper_died();
    }

    void read_helper() {
        if (!ch_.valid()) return;
        std::vector<std::pair<Json, int>> msgs;
        bool alive = ch_.read(msgs);
        for (auto& [m, f] : msgs) {
            if (f >= 0) ::close(f);
            const std::string& t = m["t"].str();
            int seq = m["seq"].as_int();
            if (t == "uploaded") {
                for (auto it = seq_keys_.begin(); it != seq_keys_.end() && it->first <= seq;) {
                    uploaded_.insert(uploaded_.end(), it->second.begin(), it->second.end());
                    it = seq_keys_.erase(it);
                }
                if (!pending_ || !pending_canvas_) canvas_busy_ = false;
            } else if (t == "shown") {
                in_flight_ = false;
                if (pending_) {
                    pending_ = false;
                    send_frame(pending_frame_, pending_keys_);
                }
            }
        }
        if (!alive) helper_died();
    }

    // The helper crashed: start it again with the same state.
    void helper_died() {
        log::error("gpu: the GPU helper stopped; restarting it");
        stop_helper();
        known_.clear();
        known_gpu_.clear();
        seq_keys_.clear();
        in_flight_ = pending_ = canvas_busy_ = false;
        int old_w = w_, old_h = h_;
        w_ = h_ = 0;
        if (++restarts_ > 3 || !start_helper()) {
            log::error("gpu: giving up on the GPU; restarting Facet with fbdev");
            // Remembered, so the next start uses the CPU until the user tries again (Settings > Graphics).
            if (!opt_.data_dir.empty())
                if (FILE* f = std::fopen((opt_.data_dir + "/gl/failed").c_str(), "w")) {
                    std::fputs("the GPU helper kept crashing", f);
                    std::fclose(f);
                }
            ::kill(getpid(), SIGTERM);  // the service restarts us; create_platform() then falls back
            w_ = old_w, h_ = old_h;
        }
    }

    static constexpr size_t kOpsCapacity = size_t(256) << 20;
    Options opt_;
    std::unique_ptr<Platform> inner_;  // test mode: input and size from another backend
    Backlight backlight_;
    EvdevTouch touch_;
    gpu::Channel ch_;
    pid_t pid_ = -1;
    bool ready_ = false;
    int w_ = 0, h_ = 0, seq_ = 0, restarts_ = 0;
    std::string renderer_, error_;
    void* canvas_ = nullptr;
    size_t canvas_size_ = 0;
    int mfd_ = -1;
    bool ops_ = false, ops_lost_ = false;
    uint8_t* ops_map_ = nullptr;
    int ops_fd_ = -1;
    bool in_flight_ = false, pending_ = false, pending_canvas_ = false, canvas_busy_ = false;
    Json pending_frame_;
    std::vector<std::string> pending_keys_, uploaded_;
    std::map<int, std::vector<std::string>> seq_keys_;
    std::set<std::string> known_, known_gpu_;  // known_gpu_: "key|slot|generation"
    bool dmabuf_ = false;  // the helper imports GPU buffers
};

std::unique_ptr<Platform> make_gpu(const Options& o, std::unique_ptr<Platform> inner, std::string& error) {
    auto p = std::make_unique<GpuPlatform>(o, std::move(inner));
    if (!p->init()) {
        error = p->error();
        return nullptr;
    }
    return p;
}

}  // namespace facet::platform
