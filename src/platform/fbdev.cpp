// Linux framebuffer backend: works from early boot, without X/Wayland.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "platform/backlight.h"
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
        if (!dev) dev = "/dev/fb0";
        fb_ = ::open(dev, O_RDWR | O_CLOEXEC);
        if (fb_ < 0) {
            log::error("fbdev: cannot open %s: %s", dev, std::strerror(errno));
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
            return false;
        }
        map_ = static_cast<uint8_t*>(m);
        fast32_ = var_.bits_per_pixel == 32 && var_.red.offset == 16 && var_.green.offset == 8 &&
                  var_.blue.offset == 0;
        log::info("fbdev: %s %ux%u %ubpp stride=%u", dev, var_.xres, var_.yres, var_.bits_per_pixel,
                  fix_.line_length);
        setup_pan();

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
    void pump(std::vector<Event>& out) override { touch_.pump(out, mono()); }

    void present(const gfx::Canvas& c) override {
        if (!pages_) {
            copy(c, var_.yoffset);
            return;
        }
        // Double buffering: draw the page that is not on screen, then show it.
        uint32_t page = pages_ > 1 ? 1 - shown_ : 0;
        copy(c, page * var_.yres);
        if (pan(page * var_.yres)) {
            shown_ = page;
        } else if (!pages_) {
            copy(c, var_.yoffset);  // pan just stopped working: draw where the screen shows
        }
    }

    void set_display_power(bool on) override {
        ioctl(fb_, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
        backlight_.set_power(on);
    }

    bool has_brightness() const override { return backlight_.available(); }
    void set_brightness(int percent) override { backlight_.set_percent(percent); }

private:
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
        if (e == EINVAL || e == ENOTTY || e == ENOSYS || e == EOPNOTSUPP) {
            log::warn("fbdev: FBIOPAN_DISPLAY failed (%s), not panning any more", std::strerror(e));
            pages_ = 0;
        } else if (!pan_warned_) {
            log::warn("fbdev: FBIOPAN_DISPLAY failed (%s)", std::strerror(e));
            pan_warned_ = true;
        }
        return false;
    }

    void copy(const gfx::Canvas& c, uint32_t yoffset) {
        int w = std::min(c.width(), width()), h = std::min(c.height(), height());
        const uint32_t* src = c.pixels();
        uint8_t* base = map_ + size_t(yoffset) * fix_.line_length + size_t(var_.xoffset) * (var_.bits_per_pixel / 8);
        for (int y = 0; y < h; ++y) {
            const uint32_t* s = src + size_t(y) * c.width();
            uint8_t* d = base + size_t(y) * fix_.line_length;
            if (fast32_) {
                std::memcpy(d, s, size_t(w) * 4);
            } else if (var_.bits_per_pixel == 32) {
                uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
                for (int x = 0; x < w; ++x) d32[x] = pack(s[x]);
            } else {
                uint16_t* d16 = reinterpret_cast<uint16_t*>(d);
                for (int x = 0; x < w; ++x) d16[x] = uint16_t(pack(s[x]));
            }
        }
    }

    uint32_t pack(uint32_t xrgb) const {
        uint32_t r = (xrgb >> 16) & 0xFF, g = (xrgb >> 8) & 0xFF, b = xrgb & 0xFF;
        return (r >> (8 - var_.red.length)) << var_.red.offset | (g >> (8 - var_.green.length)) << var_.green.offset |
               (b >> (8 - var_.blue.length)) << var_.blue.offset;
    }

    int fb_ = -1, tty_ = -1;
    fb_var_screeninfo var_{};
    fb_fix_screeninfo fix_{};
    uint8_t* map_ = nullptr;
    size_t map_size_ = 0;
    bool fast32_ = false;
    uint32_t pages_ = 0;  // 0: no panning, 1: pan in place, 2: double buffered
    uint32_t shown_ = 0;  // page on screen when double buffered
    bool pan_warned_ = false;
    Backlight backlight_;
    EvdevTouch touch_;
};

std::unique_ptr<Platform> make_fbdev() { return std::make_unique<FbdevPlatform>(); }

}  // namespace facet::platform
