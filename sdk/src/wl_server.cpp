#include "wl_server.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace facet::sdk::detail {

namespace {

// Global names in the registry.
constexpr uint32_t kCompositor = 1, kShm = 2, kWmBase = 3, kOutput = 4;
constexpr uint32_t kShmArgb = 0, kShmXrgb = 1;
constexpr int kMaxSide = 8192;

uint32_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Reads the arguments of a request in order.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    uint32_t u32() {
        if (pos_ + 4 > n_) {
            ok_ = false;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, p_ + pos_, 4);
        pos_ += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    std::string str() {
        uint32_t len = u32();
        if (len == 0) return {};  // null string
        size_t padded = (size_t(len) + 3) & ~size_t(3);
        if (!ok_ || pos_ + padded > n_) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len - 1);
        pos_ += padded;
        return s;
    }

private:
    const uint8_t* p_;
    size_t n_, pos_ = 0;
    bool ok_ = true;
};

}  // namespace

// An event: the target object, the opcode, then the arguments.
class WlServer::Message {
public:
    Message(uint32_t id, uint16_t opcode) {
        u32(id);
        u32(opcode);  // size added in bytes()
    }
    Message& u32(uint32_t v) {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        data_.insert(data_.end(), b, b + 4);
        return *this;
    }
    Message& i32(int32_t v) { return u32(uint32_t(v)); }
    Message& str(const std::string& s) {
        u32(uint32_t(s.size() + 1));
        data_.insert(data_.end(), s.begin(), s.end());
        data_.push_back(0);
        while (data_.size() % 4) data_.push_back(0);
        return *this;
    }
    Message& array(const std::vector<uint32_t>& v) {
        u32(uint32_t(v.size() * 4));
        for (uint32_t x : v) u32(x);
        return *this;
    }
    const std::vector<uint8_t>& bytes() const {
        uint32_t word;
        std::memcpy(&word, data_.data() + 4, 4);
        word = (uint32_t(data_.size()) << 16) | (word & 0xFFFF);
        std::memcpy(const_cast<uint8_t*>(data_.data()) + 4, &word, 4);
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

bool WlServer::start(const std::string& dir, int w, int h) {
    stop();
    dir_ = dir;
    w_ = std::clamp(w, 16, kMaxSide);
    h_ = std::clamp(h, 16, kMaxSide);
    path_ = dir + "/wayland-0";
    lock_path_ = path_ + ".lock";
    // libwayland's convention: a lock file beside the socket.
    lock_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_ >= 0) flock(lock_, LOCK_EX | LOCK_NB);
    ::unlink(path_.c_str());
    listen_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof addr.sun_path) {
        stop();
        return false;
    }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_, 2) != 0) {
        stop();
        return false;
    }
    return true;
}

void WlServer::stop() {
    close_client();
    if (listen_ >= 0) ::close(listen_), listen_ = -1;
    if (!path_.empty()) ::unlink(path_.c_str());
    if (lock_ >= 0) ::close(lock_), lock_ = -1;
    if (!lock_path_.empty()) ::unlink(lock_path_.c_str());
}

void WlServer::unmap_pool(Object& p) {
    if (p.map) munmap(p.map, p.size);
    if (p.fd >= 0) ::close(p.fd);
    p.map = nullptr;
    p.fd = -1;
}

void WlServer::close_client() {
    for (auto& [id, o] : objects_)
        if (o.kind == Kind::Pool) unmap_pool(o);
    objects_.clear();
    for (int fd : fds_) ::close(fd);
    fds_.clear();
    in_.clear();
    if (client_ >= 0) ::close(client_), client_ = -1;
}

void WlServer::accept_client() {
    int fd = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    if (client_ >= 0) {  // one client: a new player replaces the old connection
        close_client();
    }
    client_ = fd;
    Object display;
    display.kind = Kind::Display;
    objects_[1] = display;
    if (on_connect) on_connect();
}

void WlServer::send(const Message& m) {
    if (client_ < 0) return;
    const auto& b = m.bytes();
    size_t done = 0;
    while (done < b.size()) {
        ssize_t n = ::send(client_, b.data() + done, b.size() - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {  // events are small; a full socket means a stuck client
            usleep(1000);
            continue;
        }
        close_client();
        return;
    }
}

bool WlServer::read_client() {
    if (client_ < 0) return false;
    while (true) {
        uint8_t buf[4096];
        char control[CMSG_SPACE(sizeof(int) * 28)];
        iovec iov{buf, sizeof buf};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        ssize_t n = recvmsg(client_, &msg, MSG_CMSG_CLOEXEC);
        if (n == 0) {
            close_client();
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            close_client();
            return false;
        }
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
                for (size_t i = 0; i < count; ++i) fds_.push_back(fds[i]);
            }
        in_.insert(in_.end(), buf, buf + n);
    }
    size_t pos = 0;
    while (in_.size() - pos >= 8 && client_ >= 0) {
        uint32_t id, word;
        std::memcpy(&id, in_.data() + pos, 4);
        std::memcpy(&word, in_.data() + pos + 4, 4);
        size_t size = word >> 16;
        if (size < 8 || size % 4) {  // garbage: drop the client
            close_client();
            return false;
        }
        if (in_.size() - pos < size) break;
        handle(id, uint16_t(word & 0xFFFF), in_.data() + pos + 8, size - 8);
        pos += size;
    }
    if (client_ >= 0) in_.erase(in_.begin(), in_.begin() + long(pos));
    return client_ >= 0;
}

void WlServer::destroy(uint32_t id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return;
    if (it->second.kind == Kind::Pool) unmap_pool(it->second);
    objects_.erase(it);
    send(Message(1, 1).u32(id));  // wl_display.delete_id: the client may use it again
}

void WlServer::bind(uint32_t name, uint32_t id) {
    Object o;
    switch (name) {
        case kCompositor: o.kind = Kind::Compositor; break;
        case kShm: o.kind = Kind::Shm; break;
        case kWmBase: o.kind = Kind::WmBase; break;
        case kOutput: o.kind = Kind::Output; break;
        default: return;
    }
    objects_[id] = o;
    if (o.kind == Kind::Shm) {
        send(Message(id, 0).u32(kShmArgb));
        send(Message(id, 0).u32(kShmXrgb));
    } else if (o.kind == Kind::Output) {
        // geometry, mode (current | preferred), scale, done
        send(Message(id, 0).i32(0).i32(0).i32(0).i32(0).i32(0).str("Facet").str("surface").i32(0));
        send(Message(id, 1).u32(3).i32(w_).i32(h_).i32(60000));
        send(Message(id, 3).i32(1));
        send(Message(id, 2));
    }
}

void WlServer::configure_toplevel(uint32_t toplevel) {
    auto it = objects_.find(toplevel);
    if (it == objects_.end()) return;
    uint32_t xdg = it->second.parent;
    // A fixed size, as for a full-screen window (states: 2 = fullscreen, 4 = activated).
    send(Message(toplevel, 0).i32(w_).i32(h_).array({2, 4}));
    send(Message(xdg, 0).u32(serial_++));
}

void WlServer::resize(int w, int h) {
    w_ = std::clamp(w, 16, kMaxSide);
    h_ = std::clamp(h, 16, kMaxSide);
    for (auto& [id, o] : objects_) {
        if (o.kind == Kind::Toplevel) configure_toplevel(id);
        if (o.kind == Kind::Output) {
            send(Message(id, 1).u32(3).i32(w_).i32(h_).i32(60000));
            send(Message(id, 2));
        }
    }
}

void WlServer::commit(uint32_t sid) {
    Object& s = objects_[sid];
    if (s.role) {
        Object& xdg = objects_[s.role];
        if (!s.configured) {  // the first commit asks for the size
            s.configured = true;
            for (auto& [id, o] : objects_)
                if (o.kind == Kind::Toplevel && o.parent == s.role) configure_toplevel(id);
            (void)xdg;
        }
    }
    if (s.attach_pending) {
        s.attach_pending = false;
        uint32_t bid = s.attached;
        auto b = objects_.find(bid);
        if (bid && b != objects_.end() && b->second.kind == Kind::Buffer) {
            auto p = objects_.find(b->second.pool);
            const Object& buf = b->second;
            if (p != objects_.end() && p->second.map && on_frame) {
                const size_t need = size_t(buf.offset) + size_t(buf.stride) * size_t(buf.h);
                if (need <= p->second.size)
                    on_frame(reinterpret_cast<const uint32_t*>(p->second.map + buf.offset), buf.w, buf.h,
                             buf.stride / 4);
            }
            send(Message(bid, 0));  // wl_buffer.release: copied, the client may draw into it again
        }
    }
    // Frame callbacks: the next frame may come now.
    uint32_t t = now_ms();
    for (uint32_t cb : s.frame_callbacks) {
        send(Message(cb, 0).u32(t));
        objects_.erase(cb);
        send(Message(1, 1).u32(cb));
    }
    s.frame_callbacks.clear();
}

void WlServer::handle(uint32_t id, uint16_t op, const uint8_t* args, size_t len) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return;  // destroyed or unknown: ignore
    const Kind kind = it->second.kind;
    Reader r(args, len);
    auto create = [&](uint32_t nid, Kind k) -> Object& {
        Object& o = objects_[nid];
        o = Object{};
        o.kind = k;
        return o;
    };
    switch (kind) {
        case Kind::Display:
            if (op == 0) {  // sync
                uint32_t cb = r.u32();
                send(Message(cb, 0).u32(serial_++));
                send(Message(1, 1).u32(cb));
            } else if (op == 1) {  // get_registry
                uint32_t reg = r.u32();
                create(reg, Kind::Registry);
                send(Message(reg, 0).u32(kCompositor).str("wl_compositor").u32(4));
                send(Message(reg, 0).u32(kShm).str("wl_shm").u32(1));
                send(Message(reg, 0).u32(kWmBase).str("xdg_wm_base").u32(1));
                send(Message(reg, 0).u32(kOutput).str("wl_output").u32(2));
            }
            break;
        case Kind::Registry:
            if (op == 0) {  // bind(name, interface, version, id)
                uint32_t name = r.u32();
                r.str();
                r.u32();
                uint32_t nid = r.u32();
                if (r.ok()) bind(name, nid);
            }
            break;
        case Kind::Compositor:
            if (op == 0) create(r.u32(), Kind::Surface);
            else if (op == 1) create(r.u32(), Kind::Region);
            break;
        case Kind::Region:
            if (op == 0) destroy(id);
            break;
        case Kind::Surface: {
            Object& s = it->second;
            if (op == 0) destroy(id);
            else if (op == 1) {  // attach
                s.attached = r.u32();
                s.attach_pending = true;
            } else if (op == 3) {  // frame
                uint32_t cb = r.u32();
                create(cb, Kind::Callback);
                objects_[id].frame_callbacks.push_back(cb);
            } else if (op == 6) {
                commit(id);
            }
            break;
        }
        case Kind::Shm:
            if (op == 0) {  // create_pool(id, fd, size)
                uint32_t nid = r.u32();
                int32_t size = r.i32();
                if (fds_.empty()) {
                    close_client();
                    return;
                }
                int fd = fds_.front();
                fds_.pop_front();
                Object& p = create(nid, Kind::Pool);
                p.fd = fd;
                if (size > 0) {
                    void* m = mmap(nullptr, size_t(size), PROT_READ, MAP_SHARED, fd, 0);
                    if (m != MAP_FAILED) p.map = static_cast<uint8_t*>(m), p.size = size_t(size);
                }
            }
            break;
        case Kind::Pool: {
            if (op == 0) {  // create_buffer(id, offset, w, h, stride, format)
                uint32_t nid = r.u32();
                int32_t off = r.i32(), w = r.i32(), h = r.i32(), stride = r.i32();
                uint32_t format = r.u32();
                Object& b = create(nid, Kind::Buffer);
                b.pool = id;
                if (r.ok() && w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide && stride >= w * 4 && off >= 0 &&
                    (format == kShmArgb || format == kShmXrgb)) {
                    b.offset = off, b.w = w, b.h = h, b.stride = stride, b.format = format;
                } else {
                    b.pool = 0;  // unusable: its frames are ignored
                }
            } else if (op == 1) {
                destroy(id);
            } else if (op == 2) {  // resize
                int32_t size = r.i32();
                Object& p = it->second;
                if (p.map) munmap(p.map, p.size);
                p.map = nullptr;
                p.size = 0;
                if (size > 0 && p.fd >= 0) {
                    void* m = mmap(nullptr, size_t(size), PROT_READ, MAP_SHARED, p.fd, 0);
                    if (m != MAP_FAILED) p.map = static_cast<uint8_t*>(m), p.size = size_t(size);
                }
            }
            break;
        }
        case Kind::Buffer:
            if (op == 0) destroy(id);
            break;
        case Kind::WmBase:
            if (op == 0) destroy(id);
            else if (op == 1) create(r.u32(), Kind::Positioner);
            else if (op == 2) {  // get_xdg_surface(id, surface)
                uint32_t nid = r.u32(), surface = r.u32();
                create(nid, Kind::XdgSurface).parent = surface;
                auto s = objects_.find(surface);
                if (s != objects_.end()) s->second.role = nid;
            }
            break;
        case Kind::Positioner:
            if (op == 0) destroy(id);
            break;
        case Kind::XdgSurface:
            if (op == 0) destroy(id);
            else if (op == 1) create(r.u32(), Kind::Toplevel).parent = id;  // get_toplevel
            break;
        case Kind::Toplevel:
            if (op == 0) destroy(id);
            // set_fullscreen / maximized: the size stays the surface's anyway
            break;
        default:
            break;
    }
}

}  // namespace facet::sdk::detail
