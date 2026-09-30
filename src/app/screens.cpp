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
    draw_status_bar(W - g - touch, t.dp(28) + touch / 2);
    draw_build_line(H - t.dp(28));

    // Tiles: built-in dashboard + one per plugin that declares a tile.
    struct TileSpec {
        std::string id, title, subtitle;
        ui::Icon icon;
        ui::Tone tone;
    };
    std::vector<TileSpec> tiles;
    const std::string& lang = catalog().language();
    tiles.push_back({"__dashboard", tr("Dashboard"), tr("Clock"), ui::Icon::Clock, ui::Tone::Normal});
    for (const auto& p : host_.plugins()) {
        if (!p->m.has_tile || p->state == plugins::State::Disabled) continue;
        TileSpec s{p->m.id, p->m.tile_title.get(lang), p->tile_subtitle, ui::icon_from_name(p->m.tile_icon),
                   ui::Tone::Normal};
        if (p->state == plugins::State::Failed) s.subtitle = tr("Error"), s.tone = ui::Tone::Bad;
        else if (p->state == plugins::State::Backoff) s.subtitle = tr("Crashed, restarting…"), s.tone = ui::Tone::Warn;
        else if (p->state != plugins::State::Running) s.subtitle = tr(plugins::state_name(p->state));
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
        if (ui_.tile(s.id, r, s.title, s.subtitle, s.icon, s.tone)) {
            if (s.id == "__dashboard") navigate(View::Dashboard);
            else navigate(View::Plugin, s.id);
        }
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

void App::draw_settings(double t) {
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

    ui_.section(tr("Plugins"));
    const std::string& lang = catalog().language();
    if (host_.plugins().empty()) ui_.info(tr("No plugins found"), "", ui::Tone::Dim);
    for (const auto& p : host_.plugins()) {
        bool on = host_.is_enabled(p->m.id);
        std::string name = p->m.name.get(lang);
        if (ui_.toggle("en:" + p->m.id, name, on)) host_.set_enabled(p->m.id, on, t);
        ui::Tone tone = p->state == plugins::State::Running   ? ui::Tone::Good
                        : p->state == plugins::State::Failed  ? ui::Tone::Bad
                        : p->state == plugins::State::Backoff ? ui::Tone::Warn
                                                              : ui::Tone::Dim;
        ui_.info("  " + p->m.version, tr(plugins::state_name(p->state)), tone);
        if (p->state == plugins::State::Failed || p->state == plugins::State::Backoff)
            if (ui_.button("rs:" + p->m.id, tr("Restart “{}”", {name}))) host_.restart(p->m.id, t);
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
    if (ui_.begin_screen("plugin:" + p->m.id, p->m.name.get(catalog().language())) == ui::HeaderHit::Back)
        navigate(View::Menu);
    ui_.section(tr("Status"));
    ui_.info(tr("Plugin"), tr(plugins::state_name(p->state)),
             p->state == plugins::State::Failed ? ui::Tone::Bad : ui::Tone::Dim);
    if (!p->error.empty()) ui_.note(failure_text(p->error));
    if (p->state == plugins::State::Failed || p->state == plugins::State::Backoff)
        if (ui_.button("restart", tr("Restart"), ui::ButtonStyle::Primary)) host_.restart(p->m.id, t);
    ui_.end_screen();
}

// Draws a plugin's declarative UI tree with the same widgets as built-in
// screens. Changes are applied locally at once, then sent as events.
void App::render_plugin_ui(plugins::Plugin& p) {
    const Json& root = p.ui;
    std::string title = root["title"].as_string(p.m.name.get(catalog().language()));
    if (ui_.begin_screen("plugin:" + p.m.id, title) == ui::HeaderHit::Back) {
        navigate(View::Menu);
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

}  // namespace facet
