#include "app/images.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "core/log.h"

namespace facet {

namespace {
constexpr size_t kMaxBytes = 64u << 20;  // decoded pictures kept at most
constexpr double kRecheck = 2;          // seconds between looks at a file for changes
}  // namespace

ImageCache::ImageCache() {
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) wake_[0] = wake_[1] = -1;
    thread_ = std::thread([this] { work(); });
}

ImageCache::~ImageCache() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (int fd : wake_)
        if (fd >= 0) close(fd);
}

std::shared_ptr<gfx::Image> ImageCache::get(const std::string& path, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[path];
    e.used = now;
    if (!e.queued && now - e.checked >= kRecheck) {
        e.checked = now;
        struct stat st;
        bool exists = stat(path.c_str(), &st) == 0;
        long long mtime = exists ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : 0;
        long long size = exists ? (long long)st.st_size : -1;
        if (exists && (mtime != e.mtime || size != e.size)) {  // new or changed: decode (again)
            e.mtime = mtime, e.size = size;
            e.queued = true;
            queue_.push_back(path);
            cv_.notify_all();
        } else if (!exists) {
            e.image.reset();
            e.mtime = 0, e.size = -1;
        }
    }
    return e.image;
}

bool ImageCache::take_ready() {
    char buf[64];
    while (wake_[0] >= 0 && read(wake_[0], buf, sizeof buf) > 0) {
    }
    std::lock_guard<std::mutex> lock(mu_);
    bool r = ready_;
    ready_ = false;
    return r;
}

void ImageCache::work() {
    while (true) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            path = queue_.front();
            queue_.pop_front();
        }
        auto img = std::make_shared<gfx::Image>();
        std::string err;
        bool ok = gfx::decode_image(path, *img, err);
        if (!ok) log::warn("images: %s: %s", path.c_str(), err.c_str());
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(path);
            if (it != entries_.end()) {
                it->second.queued = false;
                it->second.failed = !ok;
                it->second.image = ok ? img : nullptr;
            }
            ready_ = true;
        }
        if (wake_[1] >= 0) {
            char c = 1;
            ssize_t n = write(wake_[1], &c, 1);
            (void)n;
        }
    }
}

void ImageCache::trim(double now) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t total = 0;
    std::vector<std::pair<double, std::string>> by_use;
    for (auto& [path, e] : entries_) {
        if (e.image) total += e.image->px.size() * 4;
        by_use.emplace_back(e.used, path);
    }
    // Paths not asked for in a minute are forgotten entirely.
    for (auto it = entries_.begin(); it != entries_.end();)
        if (!it->second.queued && now - it->second.used > 60) {
            if (it->second.image) total -= std::min(total, it->second.image->px.size() * 4);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    if (total <= kMaxBytes) return;
    std::sort(by_use.begin(), by_use.end());
    for (const auto& [used, path] : by_use) {
        if (total <= kMaxBytes) break;
        auto it = entries_.find(path);
        if (it == entries_.end() || !it->second.image || now - used < 1) continue;  // on screen now: kept
        total -= std::min(total, it->second.image->px.size() * 4);
        it->second.image.reset();
        it->second.mtime = 0, it->second.size = -1;  // decoded again if asked for
    }
}

}  // namespace facet
