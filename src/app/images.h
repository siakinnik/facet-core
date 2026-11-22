// Pictures modules show (canvas `image` ops): decoded on a worker thread,
// kept in memory up to a limit, read again when the file changes.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "gfx/image.h"

namespace facet {

class ImageCache {
public:
    ImageCache();
    ~ImageCache();
    // The picture at host path `path`, or null while it is being decoded (or
    // missing / broken). Asking again later picks up a file that appeared or changed.
    std::shared_ptr<gfx::Image> get(const std::string& path, double now);
    // True once after pictures finished decoding (the screen should be drawn again).
    bool take_ready();
    int wake_fd() const { return wake_[0]; }
    // Drops pictures not used for a while when over the memory limit. Call
    // between frames only: a frame may hold pictures until it is shown.
    void trim(double now);

private:
    struct Entry {
        std::shared_ptr<gfx::Image> image;
        long long mtime = 0, size = -1;
        double checked = -100, used = 0;
        bool queued = false, failed = false;
    };
    void work();

    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    bool stop_ = false, ready_ = false;
    std::thread thread_;
    int wake_[2] = {-1, -1};
};

}  // namespace facet
