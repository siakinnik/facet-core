#include "gpu/channel.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace facet::gpu {

void Channel::adopt(int fd, bool nonblocking) {
    close();
    fd_ = fd;
    int fl = fcntl(fd_, F_GETFL);
    fcntl(fd_, F_SETFL, nonblocking ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}

void Channel::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
    for (int f : fds_) ::close(f);
    fds_.clear();
}

bool Channel::send(Json msg, int pass_fd) {
    if (fd_ < 0) return false;
    if (pass_fd >= 0) msg["fd"] = true;
    std::string line = msg.dump() + "\n";
    size_t off = 0;
    bool first = true;
    while (off < line.size()) {
        iovec iov{line.data() + off, line.size() - off};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))] = {};
        if (first && pass_fd >= 0) {
            mh.msg_control = ctrl;
            mh.msg_controllen = sizeof ctrl;
            cmsghdr* c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
        }
        ssize_t n = ::sendmsg(fd_, &mh, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd_, POLLOUT, 0};
                ::poll(&p, 1, 1000);
                continue;
            }
            close();
            return false;
        }
        first = false;
        off += size_t(n);
    }
    return true;
}

bool Channel::fill(int timeout_ms) {
    if (timeout_ms > 0) {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, timeout_ms);
        if (r <= 0) return true;  // nothing yet, still alive
    }
    char buf[16384];
    iovec iov{buf, sizeof buf};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 8)];
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    ssize_t n = ::recvmsg(fd_, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int f;
            std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            fds_.push_back(f);
        }
    }
    if (n == 0) return false;
    in_.append(buf, size_t(n));
    return in_.size() < (8u << 20);
}

bool Channel::read(std::vector<std::pair<Json, int>>& out, int timeout_ms) {
    if (fd_ < 0) return false;
    bool alive = true;
    if (timeout_ms > 0) {
        if (in_.find('\n') == std::string::npos) alive = fill(timeout_ms);
    } else {
        // Non-blocking: drain everything available.
        while (alive) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 0) <= 0) break;
            size_t before = in_.size();
            alive = fill(0);
            if (alive && in_.size() == before) break;
        }
    }
    size_t nl;
    while ((nl = in_.find('\n')) != std::string::npos) {
        Json msg;
        bool ok = Json::parse(in_.substr(0, nl), msg);
        in_.erase(0, nl + 1);
        if (!ok) continue;
        int f = -1;
        if (msg["fd"].as_bool() && !fds_.empty()) {
            f = fds_.front();
            fds_.pop_front();
        }
        out.emplace_back(std::move(msg), f);
    }
    if (!alive) close();
    return alive;
}

}  // namespace facet::gpu
