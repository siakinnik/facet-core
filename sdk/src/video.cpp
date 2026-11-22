#include "facet/video.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "wl_server.h"

namespace facet::sdk {

namespace {

double mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Properties mpv reports changes of (observe_property ids).
const char* const kObserved[] = {"pause", "time-pos", "duration", "paused-for-cache", "idle-active", "eof-reached",
                                 "volume", "core-idle"};

}  // namespace

Video::Video(Plugin& plugin, std::string surface_id) : plugin_(plugin), id_(std::move(surface_id)) {}

Video::~Video() {
    kill_player();
    if (server_) {
        plugin_.unwatch_fd(server_->listen_fd());
        if (server_->client_fd() >= 0) plugin_.unwatch_fd(server_->client_fd());
        server_->stop();
    }
    if (!dir_.empty()) ::rmdir(dir_.c_str());
}

void Video::set_player(std::vector<std::string> command, std::vector<std::string> env) {
    if (!command.empty()) command_ = std::move(command);
    env_ = std::move(env);
}

void Video::start_server(int w, int h) {
    if (server_) {
        server_->resize(w, h);
        return;
    }
    if (dir_.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        std::string base = std::string(tmp && *tmp ? tmp : "/tmp") + "/facet-video-XXXXXX";
        std::vector<char> name(base.begin(), base.end());
        name.push_back(0);
        if (mkdtemp(name.data())) dir_ = name.data();
        char real[PATH_MAX];
        if (!dir_.empty() && realpath(dir_.c_str(), real)) dir_ = real;  // the player needs an absolute path
    }
    server_ = std::make_unique<detail::WlServer>();
    if (dir_.empty() || !server_->start(dir_, w, h)) {
        Plugin::log("video: cannot start the display for the player in %s", dir_.c_str());
        server_.reset();
        return;
    }
    server_->on_frame = [this](const uint32_t* px, int fw, int fh, int stride) { on_frame(px, fw, fh, stride); };
    server_->on_connect = [this] {
        plugin_.watch_fd(server_->client_fd(), [this] {
            int fd = server_->client_fd();
            if (!server_->read_client()) plugin_.unwatch_fd(fd);
        });
        connect_ipc();  // mpv opens its IPC socket before its window
    };
    plugin_.watch_fd(server_->listen_fd(), [this] { server_->accept_client(); });
    surface_.shown_hook_ = [this] { flush_frame(); };
}

bool Video::play(const std::string& url, int width, int height, const std::vector<std::string>& options) {
    kill_player();
    width_ = std::max(16, width);
    height_ = std::max(16, height);
    if (!surface_.valid() || surface_.width() != width_ || surface_.height() != height_) {
        if (!surface_.create(plugin_, id_, width_, height_)) {
            Plugin::log("video: no surface (shared memory unavailable)");
            return false;
        }
    }
    start_server(width_, height_);
    if (!server_) return false;

    std::vector<std::string> args = command_;
    for (const char* a : {"--vo=wlshm", "--msg-level=all=warn", "--no-input-terminal", "--idle=no", "--keep-open=no",
                          "--osc=no", "--osd-level=0", "--input-default-bindings=no", "--input-vo-keyboard=no",
                          "--force-window=yes", "--border=no", "--wayland-app-id=facet-video", "--ao=alsa,null"})
        args.push_back(a);
    args.push_back("--input-ipc-server=" + dir_ + "/mpv.sock");
    for (const auto& o : options) args.push_back(o);
    args.push_back("--");
    args.push_back(url);

    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        ::close(out[0]), ::close(out[1]);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);  // stop() ends everything the player started
        dup2(out[1], 1);
        dup2(out[1], 2);
        int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        setenv("WAYLAND_DISPLAY", (dir_ + "/wayland-0").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", dir_.c_str(), 1);
        unsetenv("DISPLAY");  // never an X server
        for (const auto& e : env_) {
            size_t eq = e.find('=');
            if (eq != std::string::npos) setenv(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str(), 1);
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "cannot run %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    ::close(out[1]);
    pid_ = pid;
    out_fd_ = out[0];
    fcntl(out_fd_, F_SETFL, O_NONBLOCK);
    plugin_.watch_fd(out_fd_, [this] { read_output(); });
    state_ = State{};
    state_.running = true;
    state_.buffering = true;
    // The IPC socket appears a moment after start: try a few times a second until it does.
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd_ >= 0) {
        itimerspec t{};
        t.it_value.tv_nsec = t.it_interval.tv_nsec = 150 * 1000 * 1000;
        timerfd_settime(timer_fd_, 0, &t, nullptr);
        plugin_.watch_fd(timer_fd_, [this] {
            uint64_t n;
            ssize_t r = ::read(timer_fd_, &n, sizeof n);
            (void)r;
            connect_ipc();
        });
    }
    notify(false);
    return true;
}

void Video::kill_player() {
    if (pid_ > 0) {
        ::kill(-pid_, SIGTERM);
        ::kill(pid_, SIGTERM);
        // Up to half a second to end by itself, then for good.
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                break;
            }
            usleep(10 * 1000);
        }
        if (pid_ > 0) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }
    for (int* fd : {&out_fd_, &ipc_fd_, &timer_fd_})
        if (*fd >= 0) {
            plugin_.unwatch_fd(*fd);
            ::close(*fd);
            *fd = -1;
        }
    out_buf_.clear();
    ipc_buf_.clear();
    ipc_queue_.clear();
    has_pending_ = false;
}

void Video::stop() {
    if (pid_ <= 0) return;
    kill_player();
    state_.running = state_.playing = state_.buffering = false;
    notify(false);
}

void Video::reap(bool wait) {
    if (pid_ <= 0) return;
    int status = 0;
    pid_t r = waitpid(pid_, &status, wait ? 0 : WNOHANG);
    if (r != pid_) return;
    pid_ = -1;
    kill_player();  // closes the rest
    state_.running = state_.playing = state_.buffering = false;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (code != 0 && state_.error.empty()) state_.error = "the player stopped (status " + std::to_string(code) + ")";
    notify(false);
    if (on_exit) on_exit(code);
}

void Video::read_output() {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(out_fd_, buf, sizeof buf);
        if (n > 0) {
            out_buf_.append(buf, size_t(n));
            size_t nl;
            while ((nl = out_buf_.find('\n')) != std::string::npos) {
                std::string line = out_buf_.substr(0, nl);
                out_buf_.erase(0, nl + 1);
                if (line.empty()) continue;
                if (on_log) on_log(line);
                else Plugin::log("[mpv] %s", line.c_str());
            }
            continue;
        }
        if (n == 0) {  // the player is gone
            plugin_.unwatch_fd(out_fd_);
            ::close(out_fd_);
            out_fd_ = -1;
            reap(true);
        }
        return;
    }
}

void Video::connect_ipc() {
    if (ipc_fd_ >= 0 || pid_ <= 0) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = dir_ + "/mpv.sock";
    if (path.size() >= sizeof addr.sun_path) {
        ::close(fd);
        return;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return;
    }
    ipc_fd_ = fd;
    if (timer_fd_ >= 0) {
        plugin_.unwatch_fd(timer_fd_);
        ::close(timer_fd_);
        timer_fd_ = -1;
    }
    plugin_.watch_fd(ipc_fd_, [this] { read_ipc(); });
    int id = 1;
    for (const char* p : kObserved) send_ipc(Json::Array{"observe_property", id++, p});
    std::vector<std::string> queued;
    queued.swap(ipc_queue_);
    for (const auto& line : queued) {
        Json j;
        if (Json::parse(line, j)) send_ipc(j);
    }
}

void Video::send_ipc(const Json& args) {
    Json msg = Json::object();
    msg["command"] = args;
    std::string line = msg.dump() + "\n";
    if (ipc_fd_ < 0) {
        ipc_queue_.push_back(args.dump());
        return;
    }
    size_t done = 0;
    while (done < line.size()) {
        ssize_t n = ::send(ipc_fd_, line.data() + done, line.size() - done, MSG_NOSIGNAL);
        if (n > 0) done += size_t(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && errno == EAGAIN) usleep(1000);
        else return;
    }
}

void Video::read_ipc() {
    char buf[8192];
    while (true) {
        ssize_t n = ::recv(ipc_fd_, buf, sizeof buf, 0);
        if (n > 0) {
            ipc_buf_.append(buf, size_t(n));
            continue;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            plugin_.unwatch_fd(ipc_fd_);
            ::close(ipc_fd_);
            ipc_fd_ = -1;
        }
        break;
    }
    size_t nl;
    while ((nl = ipc_buf_.find('\n')) != std::string::npos) {
        std::string line = ipc_buf_.substr(0, nl);
        ipc_buf_.erase(0, nl + 1);
        Json j;
        if (Json::parse(line, j)) handle_ipc(j);
    }
}

void Video::handle_ipc(const Json& msg) {
    const std::string& ev = msg["event"].str();
    if (ev == "property-change") {
        const std::string& name = msg["name"].str();
        const Json& d = msg["data"];
        bool position = false;
        if (name == "pause") state_.paused = d.as_bool();
        else if (name == "time-pos") {
            state_.position = d.is_number() ? d.as_number() : -1;
            position = true;
        }
        else if (name == "duration") state_.duration = d.is_number() ? d.as_number() : -1;
        else if (name == "paused-for-cache") state_.buffering = d.as_bool();
        else if (name == "eof-reached") state_.ended = d.as_bool();
        else if (name == "volume") state_.volume = int(d.as_number(100));
        else if (name == "core-idle") state_.playing = !d.as_bool();
        else if (name == "idle-active") {
            if (d.as_bool()) state_.playing = false;
        }
        if (name == "core-idle" && !d.as_bool()) state_.buffering = false;
        notify(position);
    } else if (ev == "end-file") {
        const std::string& reason = msg["reason"].str();
        if (reason == "error") state_.error = msg["file_error"].as_string("cannot play");
        if (reason == "eof") state_.ended = true;
        state_.playing = false;
        notify(false);
    } else if (ev == "playback-restart") {
        state_.buffering = false;
        state_.error.clear();
        notify(false);
    }
}

void Video::notify(bool position_only) {
    if (!on_state) return;
    double t = mono();
    if (position_only && t - notified_at_ < 0.25) return;
    notified_at_ = t;
    on_state(state_);
}

void Video::pause(bool on) { send_ipc(Json::Array{"set_property", "pause", on}); }

void Video::set_volume(int percent) { send_ipc(Json::Array{"set_property", "volume", std::clamp(percent, 0, 100)}); }

void Video::seek(double seconds, bool relative) {
    send_ipc(Json::Array{"seek", seconds, relative ? "relative" : "absolute"});
}

void Video::command(const Json& args) { send_ipc(args); }

void Video::resize(int width, int height) {
    width_ = std::max(16, width);
    height_ = std::max(16, height);
    if (surface_.valid() && (surface_.width() != width_ || surface_.height() != height_))
        surface_.create(plugin_, id_, width_, height_);
    if (server_) server_->resize(width_, height_);
}

// A frame from the player: into the surface if the core has taken the last
// one, else kept (only the newest) until it has.
void Video::on_frame(const uint32_t* px, int w, int h, int stride) {
    if (!surface_.valid()) return;
    if (surface_.ready()) {
        uint32_t* dst = surface_.pixels();
        const int sw = surface_.width(), sh = surface_.height();
        if (w == sw && h == sh) {
            for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * size_t(sw), px + size_t(y) * size_t(stride), size_t(w) * 4);
        } else {  // a frame drawn for the previous size: scaled to fit
            for (int y = 0; y < sh; ++y) {
                const uint32_t* row = px + size_t(y * h / sh) * size_t(stride);
                for (int x = 0; x < sw; ++x) dst[size_t(y) * size_t(sw) + size_t(x)] = row[x * w / sw];
            }
        }
        surface_.present();
        has_pending_ = false;
        return;
    }
    pending_.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        std::memcpy(pending_.data() + size_t(y) * size_t(w), px + size_t(y) * size_t(stride), size_t(w) * 4);
    pending_w_ = w, pending_h_ = h;
    has_pending_ = true;
}

void Video::flush_frame() {
    if (!has_pending_) return;
    has_pending_ = false;
    on_frame(pending_.data(), pending_w_, pending_h_, pending_w_);
}

}  // namespace facet::sdk
