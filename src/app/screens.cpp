#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/wait.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "app/app.h"
#include "core/build_info.h"
#include "core/timezone.h"
#include "ui/canvas_ops.h"
#include "i18n/i18n.h"

namespace facet {

using gfx::Align;
using gfx::Rect;
using ui::FontRole;
using ui::Theme;

namespace {

const char* kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                         "July",    "August",   "September", "October", "November", "December"};

std::tm local_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

std::string hhmm(const std::tm& tm) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string date_line(const std::tm& tm) {
    return tr("{}, {} {}", {tr(kWeekdays[tm.tm_wday]), std::to_string(tm.tm_mday), tr(kMonths[tm.tm_mon])});
}

std::string local_ips() {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "—";
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
        if (std::string(buf).rfind("127.", 0) == 0) continue;
        if (!out.empty()) out += ", ";
        out += buf;
    }
    freeifaddrs(list);
    return out.empty() ? tr("no network") : out;
}

std::string failure_text(const plugins::Failure& f) {
    std::string s = tr(f.reason);
    if (f.wait_status >= 0) {
        if (WIFEXITED(f.wait_status)) s += " (" + tr("exit code {}", {std::to_string(WEXITSTATUS(f.wait_status))}) + ")";
        else if (WIFSIGNALED(f.wait_status)) s += " (" + tr("signal {}", {strsignal(WTERMSIG(f.wait_status))}) + ")";
    }
    if (f.gave_up) s += ". " + tr("Too many crashes in a row — plugin stopped.");
    return s;
}

// Short status of a module for lists and tiles.
struct Status {
    std::string text;
    ui::Tone tone;
};

Status module_status(const plugins::Plugin& p, bool enabled) {
    using plugins::Block;
    using plugins::State;
    if (!enabled) return {tr("Off"), ui::Tone::Dim};
    switch (p.block) {
        case Block::Incompatible: return {tr("Incompatible, update needed"), ui::Tone::Bad};
        case Block::NeedsReview: return {tr("Needs permission"), ui::Tone::Warn};
        case Block::MissingDependency: return {tr("Missing dependency"), ui::Tone::Warn};
        case Block::MissingPermission: return {tr("Permission denied"), ui::Tone::Warn};
        case Block::None: break;
    }
    switch (p.state) {
        case State::Running: return p.frozen ? Status{tr("Paused"), ui::Tone::Dim} : Status{tr("Running"), ui::Tone::Good};
        case State::Failed: return {tr("Error"), ui::Tone::Bad};
        case State::Backoff: return {tr("Crashed, restarting…"), ui::Tone::Warn};
        default: return {tr(plugins::state_name(p.state)), ui::Tone::Dim};
    }
}

std::string permission_title(const std::string& perm) {
    const plugins::PermissionInfo* info = plugins::permission_info(perm);
    return info ? tr(info->title) : perm;
}

std::string permission_text(const std::string& perm) {
    const plugins::PermissionInfo* info = plugins::permission_info(perm);
    return info ? tr(info->description) : tr("Unknown permission.");
}

// Why a module is not started, in the UI language.
std::string block_text(const plugins::Plugin& p) {
    if (p.block == plugins::Block::MissingPermission && !p.block_reason.args.empty())
        return tr("Needs the “{}” permission.", {permission_title(p.block_reason.args[0])});
    if (p.block == plugins::Block::MissingDependency && !p.block_reason.args.empty()) {
        const auto& a = p.block_reason.args;
        std::string text = tr("Needs “{}”, which no installed module provides.", {a[0]});
        if (a.size() > 1 && !a[1].empty())
            text += " " + tr("Install the module that provides it: {}", {a[1]});
        else
            text += " " + tr("Install a module that provides it.");
        return text;
    }
    return tr(p.block_reason);
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

ui::Tone tone_from(const std::string& s) {
    if (s == "good") return ui::Tone::Good;
    if (s == "warn") return ui::Tone::Warn;
    if (s == "bad") return ui::Tone::Bad;
    if (s == "dim") return ui::Tone::Dim;
    return ui::Tone::Normal;
}

}  // namespace

// ------------------------------------------------------------------ splash

void App::draw_splash() {
    const Theme& t = theme_;
    auto& c = ui_.canvas();
    float W = float(c.width()), H = float(c.height());
    float logo = 44;
    Rect logo_box{0, H * 0.5f - t.dp(40), W, t.dp(60)};
    ui_.text(FontRole::Medium, logo, logo_box, "siakinnik.com", t.c.text, Align::Center);

    float bw = std::min(W * 0.5f, t.dp(280)), bh = t.dp(4);
    Rect bar{(W - bw) / 2, logo_box.bottom() + t.dp(28), bw, bh};
    c.fill_round_rect(bar, bh / 2, t.c.track);
    c.fill_round_rect({bar.x, bar.y, std::max(bh, bw * splash_progress_), bh}, bh / 2, t.c.accent);

    std::string status = tr(boot_stage_ <= 1 ? "Starting plugins" : boot_stage_ == 2 ? "Waiting for the system" : "Ready");
    ui_.text(FontRole::Regular, Theme::kSmall, {0, bar.bottom() + t.dp(14), W, t.dp(24)}, status, t.c.text_dim,
             Align::Center);
    draw_build_line(H - t.dp(34));
}

// Small dim "version · channel · commit" line (+ repository) centred at `y`.
void App::draw_build_line(float y) {
    const Theme& t = theme_;
    float W = float(ui_.canvas().width());
    std::string line = build::summary();
    if (*build::info().repo) line += std::string(" \u00b7 ") + build::info().repo;
    ui_.text(FontRole::Regular, 12, {0, y, W, t.dp(20)}, ui_.ellipsize(FontRole::Regular, 12, line, W - t.dp(40)),
             gfx::mix(t.c.text_dim, t.c.bg, 0.35f), Align::Center);
}

// Network indicator ending at x = `right`, vertically centred on `cy`.
void App::draw_status_bar(float right, float cy) {
    const Theme& t = theme_;
    net::Status st = net_.status();
    ui::NetIcon icon = st.kind == net::Kind::Wifi       ? ui::NetIcon::Wifi
                       : st.kind == net::Kind::Ethernet ? ui::NetIcon::Ethernet
                                                        : ui::NetIcon::Offline;
    std::string label = st.kind == net::Kind::Wifi       ? (st.ssid.empty() ? tr("Wi-Fi") : st.ssid)
                        : st.kind == net::Kind::Ethernet ? tr("Ethernet")
                                                         : tr("Offline");
    float is = t.dp(26);
    float tw = std::min(ui_.text_width(FontRole::Regular, Theme::kSmall, label), t.dp(220));
    float x = right - tw;
    ui_.text(FontRole::Regular, Theme::kSmall, {x, cy - t.dp(12), tw, t.dp(24)},
             ui_.ellipsize(FontRole::Regular, Theme::kSmall, label, tw + 1), t.c.text_dim);
    gfx::Color on = st.kind == net::Kind::Offline ? t.c.warn : t.c.text;
    ui::draw_net_icon(ui_.canvas(), icon, net::bars(st.signal), {x - t.dp(8) - is, cy - is / 2, is, is}, on,
                      t.c.track);
}

// ------------------------------------------------------------------ menu

void App::draw_menu() {
    const Theme& t = theme_;
    auto& c = ui_.canvas();
    float W = float(c.width()), H = float(c.height());
    float g = t.dp(28);
    std::tm tm = local_now();

    // Clock header.
    ui_.text(FontRole::Light, 64, {g, t.dp(20), W, t.dp(84)}, hhmm(tm), t.c.text);
    ui_.text(FontRole::Regular, Theme::kBody, {g + t.dp(4), t.dp(100), W, t.dp(30)}, date_line(tm), t.c.text_dim);
    float touch = t.dp(Theme::kTouch) * 1.3f;
    if (ui_.icon_button("settings", {W - g - touch + t.dp(10), t.dp(28), touch, touch}, ui::Icon::Settings))
        navigate(View::Settings);
    float status_right = W - g - touch;
    if (size_t n = host_.notifications().list().size()) {
        Rect bell{status_right - touch, t.dp(28), touch, touch};
        if (ui_.icon_button("bell", bell, ui::Icon::Bell)) navigate(View::Notifications);
        std::string count = n > 99 ? "99+" : std::to_string(n);
        float bh = t.dp(20), bw = std::max(bh, ui_.text_width(FontRole::Medium, 12, count) + t.dp(10));
        Rect b{bell.cx() + t.dp(4), bell.y + t.dp(6), bw, bh};
        c.fill_round_rect(b, bh / 2, t.c.bad);
        ui_.text(FontRole::Medium, 12, b, count, gfx::Color{255, 255, 255, 255}, Align::Center);
        status_right -= touch + t.dp(8);
    }
    draw_status_bar(status_right, t.dp(28) + touch / 2);
    draw_build_line(H - t.dp(28));

    // Tiles: built-in dashboard + one per plugin that declares a tile.
    struct TileSpec {
        std::string id, title, subtitle;
        ui::Icon icon;
        ui::Tone tone;
        std::string badge;
        const Json* icon_ops = nullptr;
    };
    std::vector<TileSpec> tiles;
    const std::string& lang = catalog().language();
    tiles.push_back({"__dashboard", tr("Dashboard"), tr("Clock"), ui::Icon::Clock, ui::Tone::Normal, {}, nullptr});
    for (const auto& p : host_.plugins()) {
        if (!p->m.has_tile || !host_.is_enabled(p->m.id)) continue;
        TileSpec s{p->m.id, p->m.tile_title.get(lang), p->tile_subtitle,
                   ui::icon_from_name(p->tile_icon_name.empty() ? p->m.tile_icon : p->tile_icon_name),
                   ui::Tone::Normal, p->badge, p->tile_icon_ops.is_array() ? &p->tile_icon_ops : nullptr};
        if (p->state != plugins::State::Running || p->block != plugins::Block::None) {
            Status st = module_status(*p, true);
            s.subtitle = st.text;
            s.tone = st.tone == ui::Tone::Dim ? ui::Tone::Normal : st.tone;
        }
        tiles.push_back(std::move(s));
    }

    float top = t.dp(160);
    float avail_w = W - 2 * g, avail_h = H - top - g;
    int cols = std::max(2, int(avail_w / t.dp(270)));
    cols = std::min<int>(cols, std::max<size_t>(2, tiles.size()));
    float gap = t.dp(16);
    float tw = (avail_w - gap * float(cols - 1)) / float(cols);
    int rows = int((tiles.size() + size_t(cols) - 1) / size_t(cols));
    float th = std::min(tw * 0.62f, (avail_h - gap * float(rows - 1)) / float(rows));
    th = std::max(th, t.dp(150));

    for (size_t i = 0; i < tiles.size(); ++i) {
        int col = int(i) % cols, row = int(i) / cols;
        Rect r{g + float(col) * (tw + gap), top + float(row) * (th + gap), tw, th};
        const auto& s = tiles[i];
        if (ui_.tile(s.id, r, s.title, s.subtitle, s.icon, s.tone, s.badge, s.icon_ops)) {
            if (s.id == "__dashboard") {
                navigate(View::Dashboard);
            } else {
                const plugins::Plugin* p = host_.find(s.id);
                if (p && p->block != plugins::Block::None) navigate(View::AppInfo, s.id);
                else open_plugin(s.id, View::Menu);
            }
        }
    }

    // Modules that cannot run (incompatible, waiting for permissions, ...) are
    // announced here, also those without a tile.
    if (int n = host_.attention_count()) {
        std::string label = tr("Modules need attention: {}", {std::to_string(n)});
        float lw = ui_.text_width(FontRole::Medium, Theme::kSmall, label) + t.dp(64);
        Rect r{(W - lw) / 2, H - t.dp(84), lw, t.dp(44)};
        ui::Context::Press pr = ui_.press("attention", r);
        c.fill_round_rect(r, r.h / 2, pr.held ? t.c.surface_pressed : t.c.surface);
        float is = t.dp(22);
        ui::draw_icon(c, ui::Icon::Warning, {r.x + t.dp(18), r.cy() - is / 2, is, is}, t.c.warn);
        ui_.text(FontRole::Medium, Theme::kSmall, {r.x + t.dp(48), r.y, r.w - t.dp(60), r.h}, label, t.c.text);
        if (pr.clicked) navigate(View::Apps);
    }
}

// ------------------------------------------------------------------ dashboard

void App::draw_dashboard() {
    const Theme& t = theme_;
    auto& c = ui_.canvas();
    float W = float(c.width()), H = float(c.height());
    std::tm tm = local_now();
    std::string time = hhmm(tm);

    // Fit the time to ~70% of the width / 42% of the height.
    gfx::Font& light = fonts_.light;
    float w100 = light.measure(time, 100);
    int px = int(std::min(W * 0.7f / w100 * 100.f, H * 0.42f / 0.72f));
    gfx::Color main = night_ ? t.c.text_dim : t.c.text;
    gfx::Color sub = night_ ? gfx::mix(t.c.text_dim, t.c.bg, 0.4f) : t.c.text_dim;

    float cap = light.cap_height(px);
    float baseline = H * 0.47f + cap * 0.5f;
    c.draw_text(light, px, (W - light.measure(time, px)) / 2, baseline, time, main);

    int dpx = std::max(t.px(20), px / 7);
    std::string date = date_line(tm);
    c.draw_text(fonts_.regular, dpx, (W - fonts_.regular.measure(date, dpx)) / 2, baseline + dpx * 2.2f, date, sub);

    if (ui_.background_tap()) navigate(View::Menu);
}

// ------------------------------------------------------------------ settings

void App::draw_settings(double) {
    if (ui_.begin_screen("settings", tr("Settings")) == ui::HeaderHit::Back) navigate(View::Menu);

    ui_.section(tr("Appearance"));
    int theme = config_.get_int("theme", 0);
    if (ui_.select("theme", tr("Theme"), {tr("Dark"), tr("Light"), tr("Auto (day/night)")}, theme))
        config_.set("theme", theme);
    if (platform_->has_brightness()) {
        int b = config_.get_int("brightness", 100);
        if (ui_.stepper("brightness", tr("Brightness"), b, 10, 100, 10, "%")) {
            config_.set("brightness", b);
            platform_->set_brightness(b);
        }
    } else {
        ui_.info(tr("Brightness"), tr("no backlight control"), ui::Tone::Dim);
    }
    std::vector<std::string> lang_names;
    int lang_index = 0;
    for (size_t i = 0; i < languages().size(); ++i) {
        lang_names.push_back(languages()[i].native_name);
        if (catalog().language() == languages()[i].code) lang_index = int(i);
    }
    if (ui_.select("language", tr("Language"), lang_names, lang_index)) set_language(languages()[size_t(lang_index)].code);

    draw_timezone_settings();

    // Keyboard: built-in or any installed keyboard plugin.
    ui_.section(tr("Input"));
    {
        std::vector<std::string> ids = {"builtin"}, names = {tr("Built-in")};
        for (const auto& p : host_.plugins())
            if (p->m.provides_cap("input.keyboard")) {
                ids.push_back(p->m.id);
                names.push_back(p->m.name.get(catalog().language()));
            }
        std::string cur = config_.get_str("keyboard", "keyboard");
        int ki = 0;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == cur) ki = int(i);
        if (ui_.select("keyboard", tr("Keyboard"), names, ki)) config_.set("keyboard", ids[size_t(ki)]);
    }

    // Modules with a settings page (e.g. screen and camera) live here.
    const std::string& lang = catalog().language();
    bool any_settings = false;
    for (const auto& p : host_.plugins()) {
        if (!p->m.has_settings || !host_.is_enabled(p->m.id)) continue;
        if (!any_settings) ui_.section(tr("Modules"));
        any_settings = true;
        Status st = module_status(*p, true);
        bool ok = p->state == plugins::State::Running && p->block == plugins::Block::None;
        if (ui_.link("ms:" + p->m.id, p->m.settings_title.get(lang), ok ? std::string() : st.text, st.tone)) {
            if (p->block != plugins::Block::None) navigate(View::AppInfo, p->m.id);
            else open_plugin(p->m.id, View::Settings);
        }
    }

    ui_.section(tr("Apps"));
    {
        int n = host_.attention_count();
        std::string value = n ? tr("need attention: {}", {std::to_string(n)})
                              : tr("installed: {}", {std::to_string(host_.plugins().size())});
        if (ui_.link("apps", tr("Installed modules"), value, n ? ui::Tone::Warn : ui::Tone::Normal))
            navigate(View::Apps);
    }

    ui_.section(tr("Network"));
    net::Status st = net_.status();
    if (st.kind == net::Kind::Offline) {
        ui_.info(tr("Connection"), tr("Offline"), ui::Tone::Warn);
    } else {
        std::string kind = st.kind == net::Kind::Wifi ? tr("Wi-Fi") : tr("Ethernet");
        if (!st.ssid.empty()) kind += " \u00b7 " + st.ssid;
        ui_.info(tr("Connection"), kind + " (" + st.iface + ")", ui::Tone::Good);
        if (st.kind == net::Kind::Wifi && st.signal >= 0) ui_.level(tr("Signal"), st.signal / 100.f, std::to_string(st.signal) + "%");
    }
    if (ip_cache_.empty()) ip_cache_ = local_ips();
    ui_.info(tr("IP address"), ip_cache_);

    ui_.section(tr("System"));
    ui_.info(tr("Display"), std::string(platform_->name()) + " · " + std::to_string(canvas_.width()) + "×" +
                                std::to_string(canvas_.height()));
    const build::Info& b = build::info();
    ui_.info(tr("Version"), b.version);
    ui_.info(tr("Build"), tr(b.channel) + (*b.date ? " \u00b7 " + std::string(b.date) : std::string()),
             std::string(b.channel) == "release" ? ui::Tone::Good : ui::Tone::Warn);
    ui_.info(tr("Commit"), *b.commit ? std::string(b.commit) + (b.dirty ? " " + tr("(modified)") : std::string())
                                     : tr("unknown"));
    ui_.info(tr("Repository"), *b.repo ? b.repo : tr("unknown"));
    ui_.note(tr("Data: {}", {paths::data_root()}));
    ui_.end_screen();
}

// Region -> city, so no list is longer than one continent.
void App::draw_timezone_settings() {
    ui_.section(tr("Date & time"));
    const std::string zone = config_.get_str("timezone", "");

    // Regions: system, UTC, then every tzdata region ("Europe", "America", ...).
    std::vector<std::string> regions = {"", "UTC"};
    for (const auto& z : tz::zones()) {
        std::string r = tz::region_of(z);
        if (r != "UTC" && std::find(regions.begin(), regions.end(), r) == regions.end()) regions.push_back(r);
    }
    std::string sys = tz::system_zone();
    std::vector<std::string> labels = {tr("System ({})", {sys.empty() ? tr("unknown") : sys}), "UTC"};
    for (size_t i = 2; i < regions.size(); ++i) labels.push_back(tr(regions[i]));

    std::string region = tz_region_pending_ ? *tz_region_pending_ : zone.empty() ? "" : tz::region_of(zone);
    int ri = int(std::find(regions.begin(), regions.end(), region) - regions.begin());
    if (ui_.select("tz_region", tr("Time zone"), labels, ri)) {
        const std::string& picked = regions[size_t(ri)];
        if (picked.empty() || picked == "UTC") set_timezone(picked);
        else tz_region_pending_ = picked;  // wait for the city
        region = picked;
    }

    if (!region.empty() && region != "UTC") {
        std::vector<std::string> cities, city_labels;
        int ci = -1;
        for (const auto& z : tz::zones()) {
            if (tz::region_of(z) != region) continue;
            if (z == zone) ci = int(cities.size());
            cities.push_back(z);
            city_labels.push_back(tz::city_label(z));
        }
        if (ui_.select("tz_city", tr("City"), city_labels, ci) && ci >= 0) set_timezone(cities[size_t(ci)]);
    }
    ui_.info(tr("Local time"), hhmm(local_now()));
}

// ------------------------------------------------------------------ plugin screen

void App::draw_plugin(double t) {
    plugins::Plugin* p = host_.find(plugin_id_);
    if (!p) {
        navigate(View::Menu);
        return;
    }
    if (p->state == plugins::State::Running && p->ui.is_object()) {
        render_plugin_ui(*p);
        return;
    }
    if (ui_.begin_screen("plugin:" + p->m.id, p->m.name.get(catalog().language())) == ui::HeaderHit::Back) {
        leave_plugin();
        ui_.end_screen();
        return;
    }
    ui_.section(tr("Status"));
    ui_.info(tr("Plugin"), tr(plugins::state_name(p->state)),
             p->state == plugins::State::Failed ? ui::Tone::Bad : ui::Tone::Dim);
    if (!p->error.empty()) ui_.note(failure_text(p->error));
    if (p->block != plugins::Block::None) ui_.note(block_text(*p));
    if (p->state == plugins::State::Failed || p->state == plugins::State::Backoff)
        if (ui_.button("restart", tr("Restart"), ui::ButtonStyle::Primary)) host_.restart(p->m.id, t);
    if (ui_.button("info", tr("About this module"))) navigate(View::AppInfo, p->m.id);
    ui_.end_screen();
}

// ------------------------------------------------------------------ apps

void App::draw_apps() {
    if (ui_.begin_screen("apps", tr("Installed modules")) == ui::HeaderHit::Back) {
        navigate(View::Settings);
        ui_.end_screen();
        return;
    }
    const std::string& lang = catalog().language();
    ui_.section({});
    if (host_.plugins().empty()) ui_.info(tr("No modules installed"), "", ui::Tone::Dim);
    for (const auto& p : host_.plugins()) {
        Status st = module_status(*p, host_.is_enabled(p->m.id));
        if (ui_.link("app:" + p->m.id, p->m.name.get(lang), st.text, st.tone)) navigate(View::AppInfo, p->m.id);
    }
    if (!host_.sandbox_active())
        ui_.note(tr("Modules run without containers because Facet does not run as root: permissions are shown "
                    "but not enforced."));

    auto background = host_.background_registry();
    if (!background.empty()) {
        ui_.section(tr("Running in the background"));
        for (const auto& b : background)
            if (ui_.link("bg:" + b.plugin, module_name(b.plugin), b.task)) navigate(View::AppInfo, b.plugin);
    }
    const auto& wayland = host_.wayland_clients();
    if (!wayland.empty()) {
        ui_.section(tr("Desktop apps (Wayland)"));
        for (const auto& w : wayland)
            ui_.info(w.title.empty() ? w.app_id : w.title, module_name(w.module) + (w.focused ? " · " + tr("on screen") : ""));
    }
    ui_.end_screen();
}

void App::draw_app_info(double t) {
    plugins::Plugin* p = host_.find(plugin_id_);
    if (!p) {
        navigate(View::Apps);
        return;
    }
    const std::string& lang = catalog().language();
    const std::string id = p->m.id;
    if (ui_.begin_screen("appinfo:" + id, p->m.name.get(lang)) == ui::HeaderHit::Back) {
        navigate(View::Apps);
        ui_.end_screen();
        return;
    }
    bool enabled = host_.is_enabled(id);
    Status st = module_status(*p, enabled);

    ui_.section(tr("Module"));
    ui_.info(tr("Status"), st.text, st.tone);
    ui_.info(tr("Version"), p->m.version);
    ui_.info(tr("Built with SDK"), p->m.sdk.empty() ? tr("unknown") : p->m.sdk,
             p->m.compatible() ? ui::Tone::Normal : ui::Tone::Bad);
    if (p->state == plugins::State::Running) ui_.info(tr("Container"), p->sandboxed ? tr("yes") : tr("no"));
    if (ui_.toggle("enabled", tr("Enabled"), enabled)) host_.set_enabled(id, enabled, t);
    if (p->state == plugins::State::Running && (p->m.has_tile || p->m.has_settings))
        if (ui_.link("open", tr("Open"))) open_plugin(id, View::AppInfo);
    if (p->block != plugins::Block::None) ui_.note(block_text(*p));
    if (!p->error.empty() && p->state != plugins::State::Running) ui_.note(failure_text(p->error));
    if (enabled && (p->state == plugins::State::Failed || p->state == plugins::State::Backoff))
        if (ui_.button("restart", tr("Restart"), ui::ButtonStyle::Primary)) host_.restart(id, t);

    if (p->m.compatible()) {
        ui_.section(tr("Permissions"));
        if (p->m.permissions.empty()) ui_.info(tr("No special permissions"), "", ui::Tone::Dim);
        for (const auto& r : p->m.permissions) {
            const plugins::PermissionInfo* info = plugins::permission_info(r.name);
            std::string title = permission_title(r.name);
            if (r.optional) title += " (" + tr("optional") + ")";
            plugins::Grant g = host_.shown_grant(id, r);
            if (!info) {
                ui_.info(title, tr("unknown to this Facet"), ui::Tone::Dim);
            } else if (r.transient) {
                std::vector<std::string> options = {tr("Allow while in use"), tr("Ask every time"), tr("Don't allow")};
                int index = g == plugins::Grant::Allow ? 0 : g == plugins::Grant::Deny ? 2 : 1;
                if (ui_.select("perm:" + r.name, title, options, index)) {
                    plugins::Grant chosen = index == 0 ? plugins::Grant::Allow
                                            : index == 2 ? plugins::Grant::Deny
                                                         : plugins::Grant::Ask;
                    host_.set_grant(id, r.name, chosen, t);
                }
                if (std::find(p->transient.begin(), p->transient.end(), r.name) != p->transient.end())
                    if (ui_.link("stop:" + r.name, tr("In use now"), tr("Stop"), ui::Tone::Warn))
                        host_.revoke_transient(id, r.name, t);
            } else {
                bool on = g == plugins::Grant::Allow;
                if (ui_.toggle("perm:" + r.name, title, on))
                    host_.set_grant(id, r.name, on ? plugins::Grant::Allow : plugins::Grant::Deny, t);
            }
        }
        for (const auto& r : p->m.permissions) {
            const plugins::PermissionInfo* info = plugins::permission_info(r.name);
            std::string text = permission_title(r.name) + ": " + permission_text(r.name);
            std::string why = r.reason.get(lang);
            if (!why.empty()) text += " " + tr("The module says: {}", {why});
            if (info && info->level == plugins::Level::Special) text += " " + tr("Special access: allow only modules you trust.");
            ui_.note(text);
        }
        if (p->block == plugins::Block::NeedsReview) {
            ui_.note(tr("Choose what to allow, then start the module. You can change this later here."));
            if (ui_.button("confirm", tr("Start the module"), ui::ButtonStyle::Primary)) host_.confirm_permissions(id, t);
        }
        if (!host_.sandbox_active())
            ui_.note(tr("Not enforced: Facet does not run as root, so modules run without containers."));
    }

    if (!p->m.provides.empty() || !p->m.needs.empty()) {
        ui_.section(tr("Dependencies"));
        std::vector<std::string> prov;
        for (const auto& c : p->m.provides) prov.push_back(c.str());
        if (!prov.empty()) ui_.info(tr("Provides"), join(prov));
        for (const auto& c : p->m.needs) {
            std::string provider;
            for (const auto& q : host_.plugins())
                if (q.get() != p && q->m.compatible() && host_.is_enabled(q->m.id))
                    for (const auto& pc : q->m.provides)
                        if (pc.name == c.name && pc.version >= c.version) provider = q->m.name.get(lang);
            ui_.info(tr("Requires {}", {c.str()}), provider.empty() ? tr("missing") : provider,
                     provider.empty() ? ui::Tone::Bad : ui::Tone::Good);
        }
    }
    ui_.end_screen();
}

// Draws a plugin's declarative UI tree with the same widgets as built-in
// screens. Changes are applied locally at once, then sent as events.
void App::render_plugin_ui(plugins::Plugin& p) {
    const Json& root = p.ui;
    if (root["fullscreen"].is_string() && p.surface(root["fullscreen"].str())) {
        draw_fullscreen_surface(p, root["fullscreen"].str());
        return;
    }
    std::string title = root["title"].as_string(p.m.name.get(catalog().language()));
    if (ui_.begin_screen("plugin:" + p.m.id, title) == ui::HeaderHit::Back) {
        leave_plugin();
        ui_.end_screen();
        return;
    }
    if (!root["items"].is_array()) p.ui["items"] = Json::array();
    Json& items = p.ui["items"];
    for (size_t i = 0; i < items.size(); ++i) {
        Json& it = items.at(i);   // written on local (optimistic) updates
        const Json& ci = it;      // reads never insert keys
        const std::string type = ci["type"].str();
        const std::string id = ci["id"].str();
        const std::string label = ci["label"].str();
        if (type == "section") {
            ui_.section(ci["title"].str());
        } else if (type == "info") {
            ui_.info(label, ci["value"].str(), tone_from(ci["tone"].str()));
        } else if (type == "note") {
            ui_.note(ci["text"].str());
        } else if (type == "toggle") {
            bool v = ci["value"].as_bool();
            if (ui_.toggle(id, label, v)) {
                it["value"] = v;
                host_.send_event(p.m.id, id, v);
            }
        } else if (type == "select") {
            std::vector<std::string> opts;
            for (const auto& o : ci["options"].items()) opts.push_back(o.str());
            int v = ci["value"].as_int();
            if (ui_.select(id, label, opts, v)) {
                it["value"] = v;
                host_.send_event(p.m.id, id, v);
            }
        } else if (type == "stepper") {
            int v = ci["value"].as_int();
            if (ui_.stepper(id, label, v, ci["min"].as_int(0), ci["max"].as_int(100), std::max(1, ci["step"].as_int(1)),
                            ci["unit"].str())) {
                it["value"] = v;
                host_.send_event(p.m.id, id, v);
            }
        } else if (type == "time") {
            int v = ci["value"].as_int();
            if (ui_.time(id, label, v, std::max(1, ci["step"].as_int(15)))) {
                it["value"] = v;
                host_.send_event(p.m.id, id, v);
            }
        } else if (type == "level") {
            ui_.level(label, float(ci["value"].as_number()), ci["text"].str());
        } else if (type == "text") {
            std::string v = ci["value"].str();
            const std::string placeholder = ci["placeholder"].str();
            ui::Context::TextOptions o;
            o.placeholder = placeholder;
            o.secure = ci["secure"].as_bool();
            o.mode = ci["mode"].str() == "number" ? ui::InputMode::Number : ui::InputMode::Text;
            o.max_length = size_t(std::clamp(ci["max"].as_int(256), 1, 4096));
            ui::Context::TextResult r = ui_.text_field(id, label, v, o);
            if (r.changed) {
                it["value"] = v;
                host_.send_event(p.m.id, id, v);
            }
            if (r.submitted) host_.send_event(p.m.id, id, v, "submit");
        } else if (type == "surface") {
            const plugins::SurfaceBuffer* sb = p.surface(id);
            float h_dp = float(ci["height"].as_number(0));
            if (h_dp <= 0) {
                float cw = ui::Context::content_width_dp(theme_, float(canvas_.width()));
                h_dp = sb ? cw * float(sb->h) / float(sb->w) : cw * 9.f / 16.f;
            }
            draw_surface(p, id, ui_.block(std::clamp(h_dp, 1.f, 4000.f)));
        } else if (type == "canvas") {
            std::string hit = ui::draw_canvas(ui_, id, ci);
            if (!hit.empty()) host_.send_event(p.m.id, id, hit);
        } else if (type == "button") {
            const std::string& st = ci["style"].str();
            auto style = st == "primary" ? ui::ButtonStyle::Primary
                         : st == "danger" ? ui::ButtonStyle::Danger
                                          : ui::ButtonStyle::Normal;
            if (ui_.button(id, label, style)) host_.send_event(p.m.id, id, true);
        }
    }
    ui_.end_screen();
}

// ------------------------------------------------------------------ surfaces

void App::draw_surface(plugins::Plugin& p, const std::string& id, const Rect& r) {
    const plugins::SurfaceBuffer* sb = p.surface(id);
    if (!sb) {
        ui_.canvas().fill_round_rect(r, theme_.dp(Theme::kRadius), theme_.c.surface);
        return;
    }
    ui_.canvas().draw_pixels(sb->pixels(), sb->w, sb->h, sb->stride / 4, r);
    track_surface_touch(p, id, r.intersect(ui_.canvas().clip()), sb->w, sb->h, false);
}

// The plugin's pixels on the whole screen. The top edge stays Facet's: a
// swipe down from it goes back, so a full-screen plugin can never trap the user.
void App::draw_fullscreen_surface(plugins::Plugin& p, const std::string& id) {
    const plugins::SurfaceBuffer* sb = p.surface(id);
    const Theme& t = theme_;
    float W = float(canvas_.width()), H = float(canvas_.height());
    canvas_.fill_rect({0, 0, W, H}, gfx::Color{0, 0, 0, 255});
    canvas_.draw_pixels(sb->pixels(), sb->w, sb->h, sb->stride / 4, {0, 0, W, H});
    float pw = t.dp(56), ph = t.dp(5);
    canvas_.fill_round_rect({(W - pw) / 2, t.dp(6), pw, ph}, ph / 2, gfx::Color{255, 255, 255, 110});
    track_surface_touch(p, id, {0, 0, W, H}, sb->w, sb->h, true);
}

void App::track_surface_touch(plugins::Plugin& p, const std::string& id, const Rect& r, int sw, int sh,
                              bool fullscreen) {
    if (r.empty()) return;
    const float edge = theme_.dp(28), swipe = float(canvas_.height()) * 0.12f;
    auto to_surface = [&](float& x, float& y) {
        x = std::clamp((pointer_.x - r.x) * float(sw) / r.w, 0.f, float(sw - 1));
        y = std::clamp((pointer_.y - r.y) * float(sh) / r.h, 0.f, float(sh - 1));
    };
    if (pointer_.pressed && !touch_.active && r.contains(pointer_.x, pointer_.y) &&
        !ui_.overlay().contains(pointer_.x, pointer_.y)) {
        touch_ = {true, fullscreen && pointer_.y < edge, p.m.id, id, pointer_.y, -1, -1};
        if (!touch_.gesture) {
            float x, y;
            to_surface(x, y);
            host_.send_touch(p.m.id, id, "down", x, y);
            touch_.last_x = x, touch_.last_y = y;
        }
        return;
    }
    if (!touch_.active || touch_.plugin != p.m.id || touch_.surface != id) return;
    if (touch_.gesture) {
        if (pointer_.down && pointer_.y - touch_.start_y > swipe) {
            touch_ = {};
            leave_plugin();
        } else if (!pointer_.down) {
            touch_ = {};
        }
        return;
    }
    float x, y;
    to_surface(x, y);
    if (pointer_.down) {
        if (x != touch_.last_x || y != touch_.last_y) host_.send_touch(p.m.id, id, "move", x, y);
        touch_.last_x = x, touch_.last_y = y;
    } else {
        host_.send_touch(p.m.id, id, "up", x, y);
        touch_ = {};
    }
}

}  // namespace facet
