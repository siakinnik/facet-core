// Linux framebuffer backend: works from early boot, without X/Wayland.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "platform/backlight.h"
#include "platform/fb_copy.h"
#include "platform/evdev_touch.h"
#include "platform/platform.h"

namespace facet::platform {

namespace {
double mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

class FbdevPlatform final : public Platform {
public:
    ~FbdevPlatform() override {
        if (map_) munmap(map_, map_size_);
        if (fb_ >= 0) ::close(fb_);
        if (tty_ >= 0) {
            ioctl(tty_, KDSETMODE, KD_TEXT);
            ::close(tty_);
        }
    }

    const char* name() const override { return "fbdev"; }

    bool init() override {
        const char* dev = std::getenv("FACET_FB");
        dev_ = dev ? dev : "/dev/fb0";
        if (!open_fb()) return false;

        // Stop the kernel console from drawing text and the cursor over us.
        const char* tty = std::getenv("FACET_TTY");
        tty_ = ::open(tty ? tty : "/dev/tty0", O_RDWR | O_CLOEXEC);
        if (tty_ < 0) log::info("fbdev: no VT (%s), kernel console not suppressed", std::strerror(errno));
        else if (ioctl(tty_, KDSETMODE, KD_GRAPHICS) < 0) log::warn("fbdev: KD_GRAPHICS failed");

        backlight_.open();
        touch_.open_devices(width(), height());
        return true;
    }

    int width() const override { return int(var_.xres); }
    int height() const override { return int(var_.yres); }

    void add_poll_fds(std::vector<pollfd>& fds) override { touch_.add_poll_fds(fds); }
    void pump(std::vector<Event>& out) override {
        // The firmware's framebuffer (efifb, simpledrm) is replaced when the
        // real graphics driver loads, often seconds after boot, while Facet
        // runs: what is drawn into the old one never shows. Its name tells.
        double now = mono();
        bool replaced = false;
        if (now - checked_at_ > 2) {
            checked_at_ = now;
            if (!name_.empty() && fb_name() != name_) {
                log::info("fbdev: %s is now %s (was %s): opening it again", dev_.c_str(), fb_name().c_str(),
                          name_.c_str());
                replaced = true;
            }
        }
        if (replaced || gone_) {
            gone_ = false;
            // A resize event either way: the app draws the whole picture again.
            if (reopen()) resized_ = true;
        }
        if (resized_) {
            resized_ = false;
            out.push_back({EventType::Resize, 0, 0, width(), height()});
        }
        touch_.pump(out, now);
    }

    void present(const gfx::Canvas& c) override {
        if (gone_ || !map_) return;  // the event pump opens the new device
        if (c.width() != width() || c.height() != height()) return;  // a resize is on its way
        if (!pages_) {
            copy(c, var_.yoffset, 0);
            return;
        }
        // Double buffering: draw the page that is not on screen, then show it.
        uint32_t page = pages_ > 1 ? 1 - shown_ : 0;
        copy(c, page * var_.yres, page);
        if (pan(page * var_.yres)) {
            shown_ = page;
        } else if (gone_) {
            return;  // the next frame goes to the new device
        } else if (!pages_) {
            shadow_[0].clear();  // the page on screen may be the other one: write all of it
            copy(c, var_.yoffset, 0);  // pan just stopped working: draw where the screen shows
        }
    }

    // Partial redraw: only the rows drawn this time and last time can differ
    // from what a page holds (with two pages, each was written two frames ago).
    void present_damage(const gfx::Canvas& c, const std::vector<gfx::Rect>& damage) override {
        std::vector<std::pair<int, int>> rows;
        for (const std::vector<gfx::Rect>* list : {&damage, static_cast<const std::vector<gfx::Rect>*>(&last_damage_)})
            for (const auto& r : *list)
                rows.push_back({int(std::floor(r.y)), int(std::ceil(r.bottom()))});
        std::sort(rows.begin(), rows.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto& r : rows) {
            if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged.push_back(r);
        }
        last_damage_ = damage;
        rows_ = &merged;
        present(c);
        rows_ = nullptr;
    }

    void set_display_power(bool on) override {
        // What the framebuffer holds may have been changed meanwhile (the
        // console, a blank): the next frame writes everything.
        if (on) shadow_[0].clear(), shadow_[1].clear();
        ioctl(fb_, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
        backlight_.set_power(on);
    }

    bool has_brightness() const override { return backlight_.available(); }
    void set_brightness(int percent) override { backlight_.set_percent(percent); }

private:
    std::string fb_name() const {
        std::string base = dev_.substr(dev_.rfind('/') + 1);  // fb0
        std::string name;
        if (FILE* f = std::fopen(("/sys/class/graphics/" + base + "/name").c_str(), "r")) {
            char buf[64] = {};
            if (std::fgets(buf, sizeof buf, f)) name = buf;
            std::fclose(f);
        }
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        return name;
    }

    bool open_fb() {
        fb_ = ::open(dev_.c_str(), O_RDWR | O_CLOEXEC);
        if (fb_ < 0) {
            log::error("fbdev: cannot open %s: %s", dev_.c_str(), std::strerror(errno));
            return false;
        }
        if (ioctl(fb_, FBIOGET_VSCREENINFO, &var_) < 0 || ioctl(fb_, FBIOGET_FSCREENINFO, &fix_) < 0) {
            log::error("fbdev: FBIOGET_*SCREENINFO failed");
            return false;
        }
        if (var_.bits_per_pixel != 32 && var_.bits_per_pixel != 16) {
            log::error("fbdev: unsupported %u bpp", var_.bits_per_pixel);
            return false;
        }
        map_size_ = size_t(fix_.line_length) * var_.yres_virtual;
        void* m = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_, 0);
        if (m == MAP_FAILED) {
            log::error("fbdev: mmap failed: %s", std::strerror(errno));
            map_ = nullptr;
            return false;
        }
        map_ = static_cast<uint8_t*>(m);
        layout_ = FbLayout::make(int(var_.bits_per_pixel), var_.red.offset, var_.red.length, var_.green.offset,
                                 var_.green.length, var_.blue.offset, var_.blue.length, var_.transp.offset,
                                 var_.transp.length);
        name_ = fb_name();
        log::info("fbdev: %s %ux%u %ubpp stride=%u (%s)", dev_.c_str(), var_.xres, var_.yres, var_.bits_per_pixel,
                  fix_.line_length, name_.empty() ? "?" : name_.c_str());
        pages_ = 0;
        shown_ = 0;
        pan_warned_ = false;
        setup_pan();
        return true;
    }

    // The device was replaced: close the old one, open the new one (it may
    // take a moment to appear), and write the next frame whole.
    bool reopen() {
        int w = width(), h = height();
        if (map_) munmap(map_, map_size_);
        map_ = nullptr;
        if (fb_ >= 0) ::close(fb_);
        fb_ = -1;
        shadow_[0].clear(), shadow_[1].clear();
        last_damage_.clear();
        for (int i = 0; i < 50; ++i) {
            if (open_fb()) {
                if (width() != w || height() != h) touch_.set_screen_size(width(), height());
                return true;
            }
            if (fb_ >= 0) ::close(fb_), fb_ = -1;
            usleep(100 * 1000);
        }
        log::error("fbdev: %s did not come back", dev_.c_str());
        return false;
    }

    // Some drivers (MediaTek's mtkfb, command-mode panels, some Qualcomm and
    // Allwinner ones) send a frame to the display controller only on
    // FBIOPAN_DISPLAY; writing the memory alone never reaches the screen.
    void setup_pan() {
        fb_var_screeninfo v = var_;
        if (ioctl(fb_, FBIOPAN_DISPLAY, &v) < 0) {
            log::info("fbdev: no panning (%s): frames are written in place", std::strerror(errno));
            return;
        }
        pages_ = var_.yres_virtual >= 2 * var_.yres ? 2 : 1;
        shown_ = pages_ > 1 && var_.yoffset == var_.yres ? 1 : 0;
        log::info("fbdev: panning after every frame%s", pages_ > 1 ? ", double buffered" : "");
    }

    // Shows the frame at row `yoffset`. On a driver without panning, stops
    // trying (one warning, no syscall per frame); returns false then.
    bool pan(uint32_t yoffset) {
        fb_var_screeninfo v = var_;
        v.yoffset = yoffset;
        if (ioctl(fb_, FBIOPAN_DISPLAY, &v) == 0) {
            var_.yoffset = yoffset;
            return true;
        }
        int e = errno;
        if (e == ENODEV) {
            // The device went away (replaced by the real graphics driver).
            log::warn("fbdev: %s is gone, opening it again", dev_.c_str());
            gone_ = true;
        } else if (e == EINTR || e == EAGAIN || e == EBUSY) {
            if (!pan_warned_) log::warn("fbdev: FBIOPAN_DISPLAY failed (%s), trying again", std::strerror(e));
            pan_warned_ = true;
        } else {
            // Anything else: never draw into a page that may not be shown.
            log::warn("fbdev: FBIOPAN_DISPLAY failed (%s), not panning any more", std::strerror(e));
            pages_ = 0;
        }
        return false;
    }

    // Writes the canvas into the framebuffer page at row `yoffset`: only what
    // changed since that page was last written (a tap or a ticking clock
    // costs a few rows, not the whole screen).
    void copy(const gfx::Canvas& c, uint32_t yoffset, uint32_t page) {
        int w = std::min(c.width(), width()), h = std::min(c.height(), height());
        const size_t bpp = var_.bits_per_pixel / 8;
        uint8_t* base = map_ + size_t(yoffset) * fix_.line_length + size_t(var_.xoffset) * bpp;
        copy_changed(c.pixels(), size_t(c.width()), w, h, shadow_[page & 1], base, fix_.line_length, layout_, rows_);
    }

    int fb_ = -1, tty_ = -1;
    fb_var_screeninfo var_{};
    fb_fix_screeninfo fix_{};
    uint8_t* map_ = nullptr;
    size_t map_size_ = 0;
    FbLayout layout_;
    std::string dev_, name_;  // the device and its driver's name when opened
    bool gone_ = false, resized_ = false;
    double checked_at_ = 0;
    std::vector<uint32_t> shadow_[2];  // what each page holds; empty: unknown
    std::vector<gfx::Rect> last_damage_;
    const std::vector<std::pair<int, int>>* rows_ = nullptr;  // rows to compare in this present
    uint32_t pages_ = 0;  // 0: no panning, 1: pan in place, 2: double buffered
    uint32_t shown_ = 0;  // page on screen when double buffered
    bool pan_warned_ = false;
    Backlight backlight_;
    EvdevTouch touch_;
};

std::unique_ptr<Platform> make_fbdev() { return std::make_unique<FbdevPlatform>(); }

}  // namespace facet::platform
