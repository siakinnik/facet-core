// System UI that modules provide: the lock screen (system.lock), the shade
// (system.shade) and screen rotation (display.rotation). The core keeps the
// rules (when to lock, what a locked screen allows, what the shade may do);
// the modules only draw and decide (the PIN is checked by the lock module).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "app/app.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "ui/icons.h"

namespace facet {

using gfx::Rect;
using ui::FontRole;
using ui::Theme;

namespace {
constexpr const char* kLockCap = "system.lock";
constexpr const char* kShadeCap = "system.shade";
}  // namespace

// ------------------------------------------------------------------ requests

void App::handle_system_requests(double t) {
    plugins::Plugin* lock = host_.system_provider(kLockCap, kLockCap);
    plugins::Plugin* shade = host_.system_provider(kShadeCap, kShadeCap);
    for (const auto& r : host_.take_requests()) {
        if (r.kind == "lock_config" || r.kind == "unlock" || r.kind == "lock") {
            if (!lock || r.plugin != lock->m.id) continue;  // only the lock module in use decides
            if (r.kind == "lock_config") {
                bool enabled = r.msg["enabled"].as_bool();
                if (enabled != config_.get_bool("lock_required", false)) {
                    config_.set("lock_required", enabled);
                    log::info("lock: %s", enabled ? "set up" : "removed");
                }
                if (!enabled && locked_) set_locked(false);
            } else if (r.kind == "unlock") {
                if (locked_) log::info("lock: unlocked");
                set_locked(false);
            } else if (lock_required()) {
                set_locked(true);
            }
        } else if (r.kind == "shade_action") {
            if (!shade || r.plugin != shade->m.id || locked_) continue;
            const std::string& a = r.msg["action"].str();
            const Json& v = r.msg["value"];
            auto serial = [&] { return uint64_t(std::strtoull(v.as_string().c_str(), nullptr, 10)); };
            if (a == "open") {
                if (plugins::Notification* n = host_.notifications().find(serial())) {
                    std::string source = n->source;
                    host_.notification_action(n->serial, "open", t);
                    open_shade(false);
                    open_module(source);
                }
            } else if (a == "dismiss") {
                host_.notification_action(serial(), "dismiss", t);
            } else if (a == "clear") {
                std::vector<uint64_t> all;
                for (const auto& n : host_.notifications().list())
                    if (!n.is_call()) all.push_back(n.serial);
                for (uint64_t s : all) host_.notification_action(s, "dismiss", t);
            } else if (a == "wifi") {
                if (wifi_started_) wifi_.set_enabled(v.as_bool());
            } else if (a == "brightness") {
                if (platform_->has_brightness()) {
                    int b = std::clamp(v.as_int(100), 10, 100);
                    config_.set("brightness", b);
                    platform_->set_brightness(b);
                }
            } else if (a == "volume") {
                change_volume(v.as_int(volume_percent_), t, false);
            } else if (a == "rotation_auto") {
                config_.set("rotation_auto", v.as_bool());
                apply_rotation();
            } else if (a == "screen_off") {
                open_shade(false);
                forced_off_ = true;
                update_display(t);
            } else if (a == "settings" || a == "wifi_settings") {
                open_shade(false);
                navigate(a == "settings" ? View::Settings : View::Wifi);
            } else if (a == "close") {
                open_shade(false);
            }
            dirty_ = true;
        } else if (r.kind == "rotation") {
            int deg = ((r.msg["value"].as_int(0) % 360) + 360) % 360;
            deg = deg / 90 * 90;
            if (deg != sensor_rotation_) {
                sensor_rotation_ = deg;
                apply_rotation();
            }
        }
    }
    // The lock module learns the state whenever its process is new.
    if (lock && lock->state == plugins::State::Running && lock->pid != lock_told_pid_) {
        lock_told_pid_ = lock->pid;
        Json m = Json::object();
        m["t"] = "locked";
        m["value"] = locked_;
        host_.send_to(lock->m.id, m);
    }
    // Locked, and the module went away for good (uninstalled): nothing can unlock.
    if (locked_ && !lock) {
        log::warn("lock: the lock screen module is gone; unlocked");
        set_locked(false);
    }
    // A rotation module that stopped no longer turns the picture.
    if (sensor_rotation_ >= 0) {
        bool running = false;
        for (const auto& p : host_.plugins())
            if (p->state == plugins::State::Running && p->m.provides_cap("display.rotation")) running = true;
        if (!running) {
            sensor_rotation_ = -1;
            apply_rotation();
        }
    }
}

// ------------------------------------------------------------------ lock

bool App::lock_required() {
    return config_.get_bool("lock_required", false) && host_.system_provider(kLockCap, kLockCap) != nullptr;
}

void App::set_locked(bool on) {
    if (on == locked_) return;
    locked_ = on;
    if (on) {
        open_shade(false);
        if (ui_.has_focus()) ui_.blur();
        if (kb_.visible) close_keyboard();
        if (power_dialog_ != PowerDialog::Going) power_dialog_ = PowerDialog::None;
        canvas_drag_ = {};
    }
    ui_.reset_interaction();
    if (plugins::Plugin* lock = host_.system_provider(kLockCap, kLockCap)) {
        Json m = Json::object();
        m["t"] = "locked";
        m["value"] = on;
        host_.send_to(lock->m.id, m);
        if (lock->state == plugins::State::Running) lock_told_pid_ = lock->pid;
    }
    dirty_ = true;
}

// The lock module's screen; until it runs, a plain "locked" screen. Never the
// screen under it: if the module fails, the device stays locked (it is
// restarted, and the user can restart it from here).
void App::draw_lock() {
    plugins::Plugin* p = host_.system_provider(kLockCap, kLockCap);
    if (p && p->state == plugins::State::Running && p->lock_ui.is_object()) {
        render_tree(*p, p->lock_ui, "lock:" + p->m.id, false);
        return;
    }
    ui_.begin_screen("lockwait", tr("Locked"), false);
    if (p && (p->state == plugins::State::Failed || p->state == plugins::State::Backoff)) {
        ui_.info(tr("Lock screen"), tr("stopped"), ui::Tone::Bad);
        if (!p->error.empty()) ui_.note(tr(p->error.reason));
        if (ui_.button("lock_restart", tr("Restart the lock screen"), ui::ButtonStyle::Primary))
            host_.restart(p->m.id, frame_t_);
    } else {
        ui_.note(tr("Starting the lock screen…"));
    }
    ui_.end_screen();
}

// ------------------------------------------------------------------ shade

void App::open_shade(bool open) {
    if (open == shade_open_) return;
    plugins::Plugin* p = host_.system_provider(kShadeCap, kShadeCap);
    if (open && (!p || p->state != plugins::State::Running || locked_)) return;
    shade_open_ = open;
    shade_pull_ = {};
    if (ui_.has_focus()) ui_.blur();
    if (kb_.visible) close_keyboard();
    ui_.reset_interaction();
    canvas_drag_ = {};
    shade_sent_.clear();
    if (p && !open) {
        Json m = Json::object();
        m["t"] = "shade_state";
        m["open"] = false;
        host_.send_to(p->m.id, m);
    }
    dirty_ = true;
}

Json App::shade_state(double t) {
    Json s = Json::object();
    s["t"] = "shade_state";
    s["open"] = true;
    Json list = Json::array();
    for (const auto& n : host_.notifications().list()) {
        Json j = Json::object();
        j["key"] = std::to_string(n.serial);
        j["app"] = notification_app(n);
        j["title"] = n.title;
        j["body"] = n.body;
        j["kind"] = n.kind;
        j["age_s"] = int(std::max(0.0, t - n.posted_at));
        j["icon"] = n.icon;
        list.push_back(j);
    }
    s["notifications"] = list;
    Json w = Json::object();
    net::Wifi::State ws = wifi_started_ ? wifi_.state() : net::Wifi::State{};
    w["available"] = wifi_started_ && ws.backend != net::Wifi::Backend::None &&
                     ws.backend != net::Wifi::Backend::Detecting;
    w["enabled"] = ws.enabled;
    w["can_toggle"] = ws.can_toggle;
    w["ssid"] = ws.current;
    s["wifi"] = w;
    Json b = Json::object();
    b["available"] = platform_->has_brightness();
    b["value"] = config_.get_int("brightness", 100);
    s["brightness"] = b;
    Json v = Json::object();
    v["available"] = volume_.available();
    v["value"] = volume_percent_;
    s["volume"] = v;
    Json r = Json::object();
    r["available"] = rotation_supported();
    r["auto"] = config_.get_bool("rotation_auto", true);
    s["rotation"] = r;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char hm[8];
    std::snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    s["time"] = hm;
    return s;
}

// The shade module's screen over everything, a handle at the bottom to close
// it (tap or swipe up), Back and Home close it too.
void App::draw_shade(double t) {
    plugins::Plugin* p = host_.system_provider(kShadeCap, kShadeCap);
    if (!p || p->state != plugins::State::Running || locked_) {
        shade_open_ = false;
        return;
    }
    Json state = shade_state(t);
    std::string dump = state.dump();
    if (dump != shade_sent_) {
        shade_sent_ = dump;
        host_.send_to(p->m.id, state);
    }
    const Theme& th = theme_;
    float W = float(canvas_.width()), H = float(canvas_.height());
    float hh = th.dp(44);
    Rect handle{0, H - hh, W, hh};
    if (!kb_.visible) ui_.set_overlay(handle);  // the content ends above the handle
    if (p->shade_ui.is_object()) {
        render_tree(*p, p->shade_ui, "shade:" + p->m.id, false);
    } else {
        ui_.begin_screen("shadewait", {}, false);
        ui_.end_screen();
    }
    ui_.begin_overlay();
    ui::Context::Press pr = ui_.press("shade_handle", handle);
    canvas_.fill_rect(handle, pr.held ? th.c.surface_pressed : th.c.surface);
    float pw = th.dp(56), ph = th.dp(5);
    canvas_.fill_round_rect({(W - pw) / 2, handle.cy() - ph / 2, pw, ph}, ph / 2, th.c.text_dim);
    ui_.end_overlay();
    if (pr.clicked) open_shade(false);
    // A swipe up that starts on the handle closes it as well.
    if (pointer_.pressed && handle.contains(pointer_.x, pointer_.y)) shade_pull_ = {true, pointer_.y};
    if (shade_pull_.active && pointer_.down && shade_pull_.start_y - pointer_.y > th.dp(48)) {
        open_shade(false);
        swallow_ = true;  // the rest of this touch
    }
    if (!pointer_.down) shade_pull_ = {};
}

// ------------------------------------------------------------------ rotation

void App::apply_rotation() {
    int want = config_.get_int("rotation", 0);
    if (config_.get_bool("rotation_auto", true) && sensor_rotation_ >= 0) want = sensor_rotation_;
    want = ((want % 360) + 360) % 360 / 90 * 90;
    if (!rotation_supported()) want = 0;
    if (panel_w_ == 0) panel_w_ = canvas_.width(), panel_h_ = canvas_.height();
    if (want == rotation_) return;
    rotation_ = want;
    bool turned = rotation_ == 90 || rotation_ == 270;
    const int lw = turned ? panel_h_ : panel_w_, lh = turned ? panel_w_ : panel_h_;
    if (platform_->rotates_itself()) {
        // The GPU helper turns it: commands are recorded at the turned size.
        canvas_.record_into(&rec_, lw, lh);
        platform_->set_rotation(rotation_);
    } else {
        canvas_.resize(lw, lh);
        if (rotation_ == 0) rotated_ = gfx::Canvas();
        else rotated_.resize(panel_w_, panel_h_);
    }
    log::info("display: rotated %d°", rotation_);
    if (ui_.has_focus()) ui_.blur();
    if (kb_.visible) close_keyboard();
    kb_rect_ = {};
    ui_.reset_interaction();
    update_theme(true);
    dirty_ = true;
}

// A point on the panel (touch) in the turned picture's coordinates.
void App::to_logical(float& x, float& y) const {
    float px = x, py = y, pw = float(panel_w_), ph = float(panel_h_);
    switch (rotation_) {
        case 90: x = py, y = pw - 1 - px; break;
        case 180: x = pw - 1 - px, y = ph - 1 - py; break;
        case 270: x = ph - 1 - py, y = px; break;
        default: break;
    }
}

}  // namespace facet
