// X11 window backend for development (WSLg, desktop Linux). The mouse acts
// as a single touch point. FACET_SIZE=WxH sets the window size.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "platform/platform.h"

namespace facet::platform {

class X11Platform final : public Platform {
public:
    ~X11Platform() override {
        if (dpy_) {
            XDestroyWindow(dpy_, win_);
            XCloseDisplay(dpy_);
        }
    }

    const char* name() const override { return "x11"; }

    bool init() override {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            log::error("x11: cannot open display");
            return false;
        }
        if (const char* s = std::getenv("FACET_SIZE")) std::sscanf(s, "%dx%d", &w_, &h_);
        int screen = DefaultScreen(dpy_);
        visual_ = DefaultVisual(dpy_, screen);
        depth_ = DefaultDepth(dpy_, screen);
        if (depth_ < 24) {
            log::error("x11: need a 24/32-bit visual, got %d", depth_);
            return false;
        }
        win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, screen), 0, 0, unsigned(w_), unsigned(h_), 0, 0,
                                   BlackPixel(dpy_, screen));
        XStoreName(dpy_, win_, "Facet");
        XSelectInput(dpy_, win_,
                     ExposureMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask | StructureNotifyMask |
                         KeyPressMask);
        wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy_, win_, &wm_delete_, 1);
        gc_ = DefaultGC(dpy_, screen);
        XMapWindow(dpy_, win_);
        XFlush(dpy_);
        log::info("x11: window %dx%d", w_, h_);
        return true;
    }

    int width() const override { return w_; }
    int height() const override { return h_; }

    void add_poll_fds(std::vector<pollfd>& fds) override { fds.push_back({ConnectionNumber(dpy_), POLLIN, 0}); }

    void pump(std::vector<Event>& out) override {
        while (XPending(dpy_)) {
            XEvent e;
            XNextEvent(dpy_, &e);
            switch (e.type) {
                case ButtonPress:
                    if (e.xbutton.button == Button1) out.push_back({EventType::Down, float(e.xbutton.x), float(e.xbutton.y)});
                    break;
                case ButtonRelease:
                    if (e.xbutton.button == Button1) out.push_back({EventType::Up, float(e.xbutton.x), float(e.xbutton.y)});
                    break;
                case MotionNotify:
                    out.push_back({EventType::Move, float(e.xmotion.x), float(e.xmotion.y)});
                    break;
                case ConfigureNotify:
                    if (e.xconfigure.width != w_ || e.xconfigure.height != h_) {
                        w_ = e.xconfigure.width;
                        h_ = e.xconfigure.height;
                        Event r{EventType::Resize};
                        r.w = w_;
                        r.h = h_;
                        out.push_back(r);
                    }
                    break;
                case Expose:
                    if (e.xexpose.count == 0) out.push_back({EventType::Resize, 0, 0, w_, h_});
                    break;
                case KeyPress: {
                    KeySym k = XLookupKeysym(&e.xkey, 0);
                    if (k == 'q') out.push_back({EventType::Quit});
                    break;
                }
                case ClientMessage:
                    if (Atom(e.xclient.data.l[0]) == wm_delete_) out.push_back({EventType::Quit});
                    break;
            }
        }
    }

    void present(const gfx::Canvas& c) override {
        if (!on_) return;
        blit(c.pixels(), c.width(), c.height());
    }

    // Simulated: a real monitor-off would switch off the developer's screen.
    void set_display_power(bool on) override {
        on_ = on;
        if (!on) {
            XSetForeground(dpy_, gc_, 0);
            XFillRectangle(dpy_, win_, gc_, 0, 0, unsigned(w_), unsigned(h_));
            XFlush(dpy_);
        }
    }

private:
    void blit(const uint32_t* px, int w, int h) {
        XImage* img = XCreateImage(dpy_, visual_, unsigned(depth_), ZPixmap, 0,
                                   reinterpret_cast<char*>(const_cast<uint32_t*>(px)), unsigned(w), unsigned(h), 32,
                                   w * 4);
        if (!img) return;
        XPutImage(dpy_, win_, gc_, img, 0, 0, 0, 0, unsigned(w), unsigned(h));
        img->data = nullptr;  // owned by the canvas
        XDestroyImage(img);
        XFlush(dpy_);
    }

    Display* dpy_ = nullptr;
    Visual* visual_ = nullptr;
    int depth_ = 24;
    Window win_ = 0;
    GC gc_ = nullptr;
    Atom wm_delete_ = 0;
    int w_ = 1280, h_ = 800;
    bool on_ = true;
};

std::unique_ptr<Platform> make_x11() { return std::make_unique<X11Platform>(); }

}  // namespace facet::platform
