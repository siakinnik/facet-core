// Headless backend for automated checks: renders off-screen and replays a
// script (FACET_SCRIPT file), one command per line:
//   wait <sec> | tap <x> <y> | down <x> <y> | move <x> <y> | up <x> <y>
//   shot <file.ppm> | quit
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "platform/platform.h"

namespace facet::platform {

namespace {
double mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

class HeadlessPlatform final : public Platform {
public:
    const char* name() const override { return "headless"; }

    bool init() override {
        if (const char* s = std::getenv("FACET_SIZE")) std::sscanf(s, "%dx%d", &w_, &h_);
        if (const char* path = std::getenv("FACET_SCRIPT")) {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line))
                if (!line.empty() && line[0] != '#') script_.push_back(line);
        }
        next_ = mono();
        return true;
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    void add_poll_fds(std::vector<pollfd>&) override {}

    void pump(std::vector<Event>& out) override {
        while (pos_ < script_.size() && mono() >= next_) {
            std::istringstream in(script_[pos_++]);
            std::string cmd;
            in >> cmd;
            float x = 0, y = 0;
            if (cmd == "wait") {
                double s = 0;
                in >> s;
                next_ = mono() + s;
            } else if (cmd == "tap") {
                in >> x >> y;
                out.push_back({EventType::Down, x, y});
                out.push_back({EventType::Up, x, y});
            } else if (cmd == "down" || cmd == "move" || cmd == "up") {
                in >> x >> y;
                out.push_back({cmd == "down" ? EventType::Down : cmd == "up" ? EventType::Up : EventType::Move, x, y});
            } else if (cmd == "shot") {
                in >> pending_shot_;
                flush_shot();  // last presented frame; put a `wait` before it
            } else if (cmd == "quit") {
                out.push_back({EventType::Quit});
            }
        }
    }

    void present(const gfx::Canvas& c) override {
        last_ = c;
        has_frame_ = true;
    }

    void set_display_power(bool on) override { log::info("headless: display %s", on ? "on" : "off"); }

    ~HeadlessPlatform() override { flush_shot(); }

private:
    void flush_shot() {
        if (pending_shot_.empty() || !has_frame_) return;
        FILE* f = std::fopen(pending_shot_.c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", last_.width(), last_.height());
            const uint32_t* px = last_.pixels();
            for (int i = 0; i < last_.width() * last_.height(); ++i) {
                unsigned char rgb[3] = {uint8_t(px[i] >> 16), uint8_t(px[i] >> 8), uint8_t(px[i])};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
            log::info("headless: wrote %s", pending_shot_.c_str());
        }
        pending_shot_.clear();
    }

    int w_ = 1280, h_ = 800;
    std::vector<std::string> script_;
    size_t pos_ = 0;
    double next_ = 0;
    std::string pending_shot_;
    gfx::Canvas last_;
    bool has_frame_ = false;
};

std::unique_ptr<Platform> make_headless() { return std::make_unique<HeadlessPlatform>(); }

}  // namespace facet::platform
