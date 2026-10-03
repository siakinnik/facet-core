#include "app/app.h"

#include <dirent.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>

#include "core/log.h"
#include "core/timezone.h"
#include "i18n/i18n.h"

namespace facet {

namespace {

volatile sig_atomic_t g_quit = 0;

void index_fonts(const std::string& dir, std::map<std::string, std::string>& out, int depth) {
    if (depth > 4) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string name = e->d_name, path = dir + "/" + name;
        if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".ttf") == 0 || name.compare(name.size() - 4, 4, ".TTF") == 0))
            out.emplace(name, path);
        else if (e->d_type == DT_DIR || e->d_type == DT_LNK || e->d_type == DT_UNKNOWN)
            index_fonts(path, out, depth + 1);
    }
    closedir(d);
}
void on_signal(int) { g_quit = 1; }

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::tm local_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

constexpr double kTouchHold = 5;       // screen stays on at least this long after a touch
constexpr double kIdleToMenu = 120;    // sub-screens fall back to the menu
constexpr double kMinSplash = 1.5;
constexpr double kPluginSettle = 6;
constexpr int kDayStartHour = 7, kNightStartHour = 21;

// Blocks until systemd reports the boot finished (or there is no systemd).
void wait_for_system(std::atomic<bool>& ready, std::atomic<bool>& stop) {
    if (std::getenv("FACET_SKIP_BOOT_WAIT")) {
        ready = true;
        return;
    }
    double start = now_s();
    while (!stop && now_s() - start < 90) {
        std::string state;
        if (FILE* f = popen("systemctl is-system-running 2>/dev/null", "r")) {
            char buf[64] = {};
            if (std::fgets(buf, sizeof buf, f)) state = buf;
            pclose(f);
        }
        while (!state.empty() && (state.back() == '\n' || state.back() == ' ')) state.pop_back();
        if (state != "initializing" && state != "starting") {
            log::info("boot: system state '%s'", state.empty() ? "unknown" : state.c_str());
            break;
        }
        for (int i = 0; i < 5 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ready = true;
}

}  // namespace

App::~App() {
    stop_ = true;
    if (boot_thread_.joinable()) boot_thread_.join();
}

// ------------------------------------------------------------------ init

bool App::load_fonts() {
    // Font file names differ less between distributions than directories do,
    // so index every .ttf under the usual roots and pick by file name.
    std::vector<std::string> roots;
    if (const char* d = std::getenv("FACET_FONT_DIR")) roots.push_back(d);
    std::string exe_dir = paths::exe_dir();
    roots.push_back(exe_dir + "/fonts");
    roots.push_back(exe_dir + "/../share/facet/fonts");
    if (const char* home = std::getenv("HOME")) roots.push_back(std::string(home) + "/.local/share/fonts");
    roots.push_back("/usr/local/share/fonts");
    roots.push_back("/usr/share/fonts");
    std::map<std::string, std::string> index;  // file name -> first path found
    for (const auto& r : roots) index_fonts(r, index, 0);

    struct Family {
        const char *regular, *medium, *light;
    };
    static const Family kFamilies[] = {
        {"regular.ttf", "medium.ttf", "light.ttf"},  // bundled with the install
        {"OpenSans-Regular.ttf", "OpenSans-Semibold.ttf", "OpenSans-Light.ttf"},
        {"NotoSans-Regular.ttf", "NotoSans-Medium.ttf", "NotoSans-Light.ttf"},
        {"Ubuntu-R.ttf", "Ubuntu-M.ttf", "Ubuntu-L.ttf"},
        {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-ExtraLight.ttf"},
        {"LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Regular.ttf"},
    };
    auto find = [&](const char* name) {
        auto it = index.find(name);
        return it == index.end() ? std::string() : it->second;
    };
    for (const auto& f : kFamilies) {
        std::string regular = find(f.regular);
        if (regular.empty() || !fonts_.regular.load(regular)) continue;
        if (!fonts_.medium.load(find(f.medium))) fonts_.medium.load(regular);
        if (!fonts_.light.load(find(f.light))) fonts_.light.load(regular);
        log::info("fonts: %s", regular.c_str());
        return true;
    }
    log::error("fonts: no usable TrueType font found; install Open Sans, Noto Sans or DejaVu "
               "(e.g. fonts-dejavu-core) or put regular/medium/light.ttf into %s/fonts",
               exe_dir.c_str());
    return false;
}

void App::update_theme(bool force) {
    int mode = config_.get_int("theme", 0);  // 0 dark, 1 light, 2 auto
    std::tm tm = local_now();
    night_ = tm.tm_hour < kDayStartHour || tm.tm_hour >= kNightStartHour;
    bool dark = mode == 0 || (mode == 2 && night_);
    float scale = std::clamp(float(std::min(canvas_.width(), canvas_.height())) / 480.f, 0.75f, 4.f);
    if (const char* s = std::getenv("FACET_SCALE")) scale = float(std::atof(s));
    if (force || dark != theme_.dark || scale != theme_.scale) {
        theme_ = ui::Theme::make(dark, scale, &fonts_);
        host_.set_theme(dark ? "dark" : "light");
        host_.set_content_width(int(ui::Context::content_width_dp(theme_, float(canvas_.width()))));
        host_.set_screen_size(canvas_.width(), canvas_.height());
        dirty_ = true;
    }
}

bool App::init() {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    debug_input_ = std::getenv("FACET_DEBUG_INPUT") != nullptr;
    debug_frames_ = std::getenv("FACET_DEBUG_FRAMES") != nullptr;
    paths::mkdirs(paths::data_root());
    config_.load(paths::data_root() + "/config.json");

    platform::Options popt;
    popt.gpu = config_.get_str("graphics", "cpu") == "gpu";
    popt.card = config_.get_str("graphics_card", "");
    if (popt.card.empty())
        for (const auto& g : gpu::detect_gpus())
            if (g.supported) {
                popt.card = g.card;
                break;
            }
    popt.gl_dir = paths::data_root() + "/gl/current";
    popt.data_dir = paths::data_root();
    platform_ = platform::create_platform(popt);
    if (!platform_->init()) return false;
    ops_ = platform_->supports_ops();
    if (ops_)
        canvas_.record_into(&rec_, platform_->width(), platform_->height());
    else if (uint32_t* mem = platform_->canvas_memory())
        canvas_.use_external(mem, platform_->width(), platform_->height());
    else
        canvas_.resize(platform_->width(), platform_->height());
    layers_ = platform_->supports_layers();
    host_.set_gpu_layers(layers_);
    if (ops_ && platform_->supports_gpu_buffers()) {
        // Plugins may hand the GPU their GPU-drawn frames (Wayland apps).
        const char* dev = std::getenv("FACET_GPU_DEVICE");
        std::string node = dev ? dev : gpu::render_node(popt.card);
        struct stat st;
        if (!node.empty() && ::stat(node.c_str(), &st) == 0) host_.set_gpu_buffers(true, node, uint64_t(st.st_rdev));
    }
    vram_budget_ = uint64_t(std::max(0, config_.get_int("graphics_vram_mb", 0))) << 20;
    gpus_ = gpu::detect_gpus();
    gl_ = std::make_unique<gpu::GlPackage>(paths::data_root() + "/gl");
    set_language(config_.get_str("language", detect_language()), false);
    {
        std::string zone = config_.get_str("timezone", "");
        if (!tz::apply(zone)) zone.clear();
        host_.set_timezone(zone);  // plugins also inherit TZ from our environment
    }
    if (!load_fonts()) return false;
    update_theme(true);
    // The console may have blanked the panel (idle blank timer) before we started.
    platform_->set_display_power(true);
    if (platform_->has_brightness()) platform_->set_brightness(config_.get_int("brightness", 100));

    double t = now_s();
    splash_start_ = last_tick_ = last_input_ = t;
    boot_stage_ = 0;
    run_frame(t);  // splash is on screen before anything slow happens
    present_frame(true);

    host_.scan();
    host_.start_enabled(t);
    boot_stage_ = 1;
    boot_thread_ = std::thread(wait_for_system, std::ref(system_ready_), std::ref(stop_));
    log::info("facet: %s backend, %dx%d, data in %s", platform_->name(), canvas_.width(), canvas_.height(),
              paths::data_root().c_str());
    return true;
}

// ------------------------------------------------------------------ loop

int App::run() {
    if (!init()) return 1;
    std::vector<pollfd> fds;
    std::vector<platform::Event> events;

    while (!g_quit) {
        double t = now_s();
        double next = std::min(host_.next_deadline(t), t + 1.0);
        if (banner_) next = std::min(next, banner_until_);
        std::tm tm = local_now();
        next = std::min(next, t + (60 - tm.tm_sec) + 0.05);  // minute boundary for the clock
        // A dark screen draws nothing: pending redraws wait for it to wake up
        // (waking marks the frame dirty anyway), otherwise poll() would spin.
        bool animating = display_on_ && (ui_.wants_redraw() || view_ == View::Splash);
        if (animating) next = std::min(next, t + 1.0 / 60);
        if (gl_ && gl_->busy()) next = std::min(next, t + 0.5);  // download progress
        // While the GPU still reads the last canvas, drawing waits (its
        // message wakes the loop).
        bool redraw_now = dirty_ && display_on_ && platform_->ready_to_draw();
        int timeout = redraw_now ? 0 : std::max(0, int(std::ceil((next - t) * 1000)));

        fds.clear();
        platform_->add_poll_fds(fds);
        host_.add_poll_fds(fds);
        if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            log::error("poll: %s", std::strerror(errno));
            break;
        }

        t = now_s();
        events.clear();
        platform_->pump(events);
        host_.process(t);
        if (host_.take_changed()) dirty_ = true;
        bool surface_frames = host_.take_surface_frames();
        // Surfaces over the video memory limit are part of the canvas: their frames redraw it.
        if (surface_frames && cpu_surface_frames_) dirty_ = true;
        take_plugin_input();
        drew_ = false;
        handle_events(events, t);
        tick(t);
        double t0 = debug_frames_ ? now_s() : 0;
        if ((dirty_ || ui_.wants_redraw()) && display_on_ && platform_->ready_to_draw()) run_frame(t);
        double t1 = debug_frames_ ? now_s() : 0;
        if (drew_ && display_on_) present_frame(true);
        if (debug_frames_ && drew_) {
            ++stat_frames_;
            stat_draw_ += t1 - t0;
            stat_present_ += now_s() - t1;
        }
        if (debug_frames_ && t - stat_since_ >= 5) {
            if (stat_frames_ > 0)
                log::info("frames: %.1f fps, drawing %.1f ms, presenting %.1f ms per frame",
                          stat_frames_ / (t - stat_since_), stat_draw_ * 1000 / stat_frames_,
                          stat_present_ * 1000 / stat_frames_);
            stat_frames_ = 0;
            stat_draw_ = stat_present_ = 0;
            stat_since_ = t;
        }
        if (drew_) {
            cpu_surface_frames_ = !cpu_surfaces_.empty();
            for (const auto& [owner, id] : cpu_surfaces_) host_.ack_surface(owner, id);  // copied already
        }
        else if (surface_frames && display_on_ && layers_) present_frame(false);  // only the surfaces moved on
        if (ops_unsent_ && !drew_ && display_on_ && platform_->ready_to_draw()) present_frame(true);
        // Surface frames the GPU has taken: their plugins may draw the next ones.
        for (const auto& key : platform_->take_uploaded()) {
            size_t slash = key.find('/'), hash = key.rfind('#');
            if (slash != std::string::npos && hash != std::string::npos && hash > slash)
                host_.ack_surface(key.substr(0, slash), key.substr(slash + 1, hash - slash - 1));
        }
        config_.save_if_dirty(t);
    }

    log::info("facet: shutting down");
    host_.shutdown_all();
    config_.save_if_dirty(now_s(), true);
    platform_->set_display_power(true);
    return 0;
}

// The service manager starts Facet again (Restart=always).
void App::request_restart() {
    log::info("facet: restarting to apply the graphics settings");
    g_quit = 1;
}

// The (up to three) buffers the screen shows and the canvas texture, or
// with GPU drawing the mask atlas instead of the canvas.
uint64_t App::vram_base() const {
    if (!layers_) return 0;
    uint64_t screen = uint64_t(canvas_.width()) * uint64_t(canvas_.height()) * 4;
    if (ops_) return screen * 3 + uint64_t(rec_.atlas_pages()) * gfx::ops::kAtlasSize * gfx::ops::kAtlasSize;
    return screen * 4;
}

void App::present_frame(bool canvas_changed) {
    if (!layers_) {
        platform_->present(canvas_);
        return;
    }
    std::vector<platform::Layer> layers;
    for (const auto& ref : layer_refs_) {
        plugins::Plugin* o = host_.find(ref.owner);
        const plugins::SurfaceBuffer* sb = o ? o->surface(ref.id) : nullptr;
        if (!sb) {
            // Recorded commands refer to layers by number: keep the place.
            if (ops_) layers.emplace_back();
            continue;
        }
        platform::Layer l;
        l.key = ref.owner + "/" + ref.id + "#" + std::to_string(sb->generation);
        l.path = host_.surface_file(ref.owner, ref.id);
        l.w = sb->w, l.h = sb->h, l.stride = sb->stride, l.buffers = sb->buffers, l.buffer = sb->current;
        l.dst = ref.dst;
        l.clip = ref.clip;
        if (sb->gpu_current >= 0) {
            const plugins::GpuBuffer& g = sb->gpu[sb->gpu_current];
            l.gpu = sb->gpu_current;
            l.gpu_fd = g.fd;
            l.gpu_format = g.format;
            l.gpu_modifier = g.modifier;
            l.gpu_offset = g.offset;
            l.gpu_stride = g.stride;
            l.gpu_generation = g.generation;
            l.gpu_w = g.w;
            l.gpu_h = g.h;
            // Smaller than the surface: only its top left part.
            l.dst.w = ref.dst.w * float(g.w) / float(sb->w);
            l.dst.h = ref.dst.h * float(g.h) / float(sb->h);
            l.clip = l.clip.intersect(l.dst);
        }
        layers.push_back(l);
    }
    if (ops_) {
        // The helper may still read the last commands: new ones wait for it
        // (frames drawn meanwhile replace this one).
        if (canvas_changed && !platform_->ready_to_draw()) {
            ops_unsent_ = true;
            return;
        }
        static const std::vector<uint8_t> kSame;  // the previous commands again
        platform_->present_ops(canvas_changed ? rec_.data() : kSame, layers);
        if (canvas_changed) {
            rec_.sent();
            ops_unsent_ = false;
        }
        return;
    }
    platform_->present_layers(canvas_, canvas_changed, layers);
}

void App::run_frame(double t) {
    dirty_ = false;  // navigation inside the frame sets it again
    layer_refs_.clear();
    cpu_surfaces_.clear();
    frame_vram_ = vram_base();
    if (ops_) rec_.begin_frame(platform_->take_ops_lost());
    moved_ = false;
    update_theme(false);
    ui_.begin_frame(canvas_, theme_, pointer_, t);
    ui_.set_overlay(kb_rect_);  // keyboard area of the previous frame
    prepare_overlays(t);        // dialogs, calls and banners take precedence
    surface_owners_.clear();
    switch (view_) {
        case View::Splash: draw_splash(); break;
        case View::Menu: draw_menu(); break;
        case View::Dashboard: draw_dashboard(); break;
        case View::Settings: draw_settings(t); break;
        case View::Graphics: draw_graphics(); break;
        case View::Plugin: draw_plugin(t); break;
        case View::Apps: draw_apps(); break;
        case View::AppInfo: draw_app_info(t); break;
        case View::Notifications: draw_notifications(t); break;
    }
    draw_keyboard();
    draw_overlays(t);
    ui_.end_frame();
    // Keys of the built-in keyboard reach the field on the next frame.
    for (auto& [action, text] : kb_pending_) apply_key(action, text);
    if (!kb_pending_.empty()) dirty_ = true;
    kb_pending_.clear();
    drawn_minute_ = local_now().tm_min;
    drew_ = true;
    dirty_ = dirty_ || ui_.wants_redraw();
}

void App::handle_events(const std::vector<platform::Event>& events, double t) {
    using platform::EventType;
    for (const auto& e : events) {
        switch (e.type) {
            case EventType::Quit: g_quit = 1; break;
            case EventType::Resize:
                if (e.w > 0 && (e.w != canvas_.width() || e.h != canvas_.height())) {
                    canvas_.resize(e.w, e.h);
                    update_theme(true);
                }
                dirty_ = true;
                break;
            case EventType::Down:
            case EventType::Up:
            case EventType::Move: {
                if (debug_input_ && e.type != EventType::Move)
                    log::info("input: %s %.0f,%.0f", e.type == EventType::Down ? "down" : "up", e.x, e.y);
                last_input_ = t;
                if (e.type == EventType::Down) {
                    last_touch_ = t;
                    if (t - last_activity_sent_ > 1) {
                        host_.broadcast_activity();
                        last_activity_sent_ = t;
                    }
                }
                // A touch on a dark screen only wakes it; it never reaches the UI.
                if (!display_on_ || swallow_) {
                    if (e.type == EventType::Down) {
                        swallow_ = true;
                        update_display(t);
                    }
                    if (e.type == EventType::Up) swallow_ = false;
                    break;
                }
                pointer_.x = e.x;
                pointer_.y = e.y;
                // These frames cannot wait for the loop; drawn into memory the
                // GPU reads, they wait for it to finish the last one.
                if (layers_ && !ops_ && (e.type == EventType::Down || e.type == EventType::Up))
                    platform_->wait_ready(100);
                if (e.type == EventType::Down) {
                    pointer_.down = true;
                    pointer_.pressed = true;
                    run_frame(t);
                    pointer_.pressed = false;
                } else if (e.type == EventType::Up) {
                    if (!pointer_.down) break;
                    if (moved_) run_frame(t);  // let the UI see the drag before the release
                    pointer_.down = false;
                    pointer_.released = true;
                    run_frame(t);
                    pointer_.released = false;
                } else if (pointer_.down) {
                    dirty_ = true;
                    moved_ = true;
                }
                break;
            }
        }
    }
}

void App::tick(double t) {
    double dt = t - last_tick_;
    last_tick_ = t;

    if (view_ == View::Splash) {
        if (boot_stage_ == 1 && (host_.all_settled() || t - splash_start_ > kPluginSettle)) boot_stage_ = 2;
        if (boot_stage_ == 2 && system_ready_) boot_stage_ = 3;
        float target = boot_stage_ == 1 ? 0.45f : boot_stage_ == 2 ? 0.75f : 1.f;
        if (splash_progress_ < target) {
            splash_progress_ = std::min(target, splash_progress_ + float(dt) * 1.6f);
            dirty_ = true;
        }
        if (boot_stage_ == 3 && splash_progress_ >= 1.f && t - splash_start_ > kMinSplash) navigate(View::Menu);
        return;
    }

    update_gfx_dialog();
    // Settings > Graphics follows the download: progress twice a second, and every change of phase.
    if (view_ == View::Graphics) {
        int phase = int(gl_->status().phase) * 2 + (gl_->busy() ? 1 : 0);
        if (phase != gfx_phase_ || (gl_->busy() && t - gfx_refresh_ >= 0.5)) {
            gfx_phase_ = phase;
            gfx_refresh_ = t;
            dirty_ = true;
        }
    }
    if ((view_ == View::Settings || view_ == View::Graphics || view_ == View::Plugin || view_ == View::Apps ||
         view_ == View::AppInfo || view_ == View::Notifications) &&
        t - last_input_ > kIdleToMenu && !modal_active(t))
        navigate(View::Menu);
    if ((view_ == View::Menu || view_ == View::Dashboard) && local_now().tm_min != drawn_minute_) dirty_ = true;
    if (net_.take_changed() && (view_ == View::Menu || view_ == View::Settings)) dirty_ = true;
    if (banner_ && t >= banner_until_) dirty_ = true;  // take it down
    update_display(t);
}

void App::update_display(double t) {
    bool want = true;
    if (view_ != View::Splash) {
        if (auto policy = host_.display_policy(t)) want = *policy;
        if (t - last_touch_ < kTouchHold) want = true;
    }
    if (want == display_on_) return;
    display_on_ = want;
    log::info("display: %s", want ? "on" : "off");
    platform_->set_display_power(want);
    if (want) dirty_ = true;
}

void App::set_language(const std::string& lang, bool remember) {
    catalog().set_language(lang);
    if (remember) config_.set("language", catalog().language());
    host_.set_locale(catalog().language());
    dirty_ = true;
}

void App::set_timezone(const std::string& zone) {
    std::string applied = tz::apply(zone) ? zone : std::string();
    config_.set("timezone", applied);
    host_.set_timezone(applied);
    tz_region_pending_.reset();
    log::info("timezone: %s", applied.empty() ? "system" : applied.c_str());
    dirty_ = true;
}

void App::open_plugin(const std::string& id, View back) {
    navigate(View::Plugin, id);
    plugin_back_ = back;
}

void App::leave_plugin() {
    View back = plugin_back_;
    navigate(back, back == View::AppInfo ? plugin_id_ : std::string());
}

void App::navigate(View v, const std::string& plugin_id) {
    if (view_ == View::Plugin && (v != View::Plugin || plugin_id != plugin_id_)) host_.set_visible(plugin_id_, false);
    view_ = v;
    plugin_id_ = plugin_id;
    if (v == View::Plugin) host_.set_visible(plugin_id, true);
    plugin_back_ = View::Menu;
    ui_.reset_interaction();
    ip_cache_.clear();
    tz_region_pending_.reset();
    dirty_ = true;
}

}  // namespace facet
