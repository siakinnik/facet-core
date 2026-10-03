// What appears above every screen: runtime permission prompts, incoming
// calls, notification banners and the "in use" indicator for transient
// permissions; plus the notification list behind the bell.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app/app.h"
#include "i18n/i18n.h"

namespace facet {

using gfx::Align;
using gfx::Color;
using gfx::Rect;
using ui::FontRole;
using ui::Theme;

namespace {

constexpr double kBannerTime = 5;

const Color kWhite{255, 255, 255, 255};

std::string ago(double seconds) {
    if (seconds < 60) return tr("now");
    if (seconds < 3600) return tr("{} min", {std::to_string(int(seconds / 60))});
    return tr("{} h", {std::to_string(int(seconds / 3600))});
}

}  // namespace

std::string App::module_name(const std::string& id) const {
    for (const auto& p : host_.plugins())
        if (p->m.id == id) return p->m.name.get(catalog().language());
    return id;
}

std::string App::notification_app(const plugins::Notification& n) const {
    return n.app_name.empty() ? module_name(n.source) : n.app_name;
}

ui::Icon App::notification_icon(const plugins::Notification& n) const {
    if (!n.icon.empty()) return ui::icon_from_name(n.icon);
    for (const auto& p : host_.plugins())
        if (p->m.id == n.source && p->m.has_tile) return ui::icon_from_name(p->m.tile_icon);
    return n.is_call() ? ui::Icon::Phone : ui::Icon::Bell;
}

// Opens the module a notification belongs to, if it has a screen.
void App::open_module(const std::string& id) {
    const plugins::Plugin* p = host_.find(id);
    if (p && (p->m.has_tile || p->m.has_settings) && p->block == plugins::Block::None) open_plugin(id, View::Menu);
}

gfx::Rect App::banner_rect() const {
    const Theme& t = theme_;
    float W = float(canvas_.width());
    float w = std::min(W - 2 * t.dp(16), t.dp(620));
    return {(W - w) / 2, t.dp(12), w, t.dp(88)};
}

bool App::modal_active(double t) {
    return host_.pending_prompt() || host_.notifications().ringing(t) || gfx_dialog_ != GfxDialog::None;
}

// Called before the screen is drawn: what part of the screen belongs to overlays.
void App::prepare_overlays(double t) {
    if (view_ == View::Splash) return;
    if (modal_active(t)) {
        if (ui_.has_focus()) ui_.blur();  // no keyboard under a dialog
        ui_.set_overlay({0, 0, float(canvas_.width()), float(canvas_.height())});
        return;
    }
    if (banner_ && t >= banner_until_) banner_.reset();
    if (!banner_ && !kb_.visible && display_on_) {
        if (auto n = host_.notifications().next_banner()) {
            banner_ = *n;
            banner_until_ = t + kBannerTime;
        }
    }
    if (banner_ && !kb_.visible) ui_.set_overlay(banner_rect());
}

void App::draw_overlays(double t) {
    if (view_ == View::Splash) return;
    draw_indicators();
    ui_.begin_overlay();
    if (const plugins::PermissionPrompt* q = host_.pending_prompt()) {
        draw_permission_prompt(*q, t);
    } else if (plugins::Notification* call = host_.notifications().ringing(t)) {
        draw_call(*call, t);
    } else if (gfx_dialog_ != GfxDialog::None) {
        draw_gfx_dialog();
    } else if (banner_ && !kb_.visible) {
        draw_banner(t);
    }
    ui_.end_overlay();
}

std::string App::draw_dialog(const std::string& key, const std::string& title, const std::string& text,
                             const std::vector<std::pair<std::string, std::string>>& buttons) {
    const Theme& th = theme_;
    float W = float(canvas_.width()), H = float(canvas_.height());
    canvas_.fill_rect({0, 0, W, H}, th.c.bg.alpha(0.85f));
    float w = std::min(W - th.dp(32), th.dp(560));
    auto lines = ui_.wrap(FontRole::Medium, 22, title, w - th.dp(48));
    auto body = ui_.wrap(FontRole::Regular, Theme::kSmall, text, w - th.dp(48));
    float bh = th.dp(56), gap = th.dp(10);
    float h = th.dp(28) + float(lines.size()) * th.dp(32) + th.dp(8) + float(body.size()) * th.dp(22) + th.dp(22) +
              float(buttons.size()) * (bh + gap) + th.dp(14);
    Rect card{(W - w) / 2, (H - h) / 2, w, h};
    canvas_.fill_round_rect(card, th.dp(Theme::kRadius + 6), th.c.surface);
    float y = card.y + th.dp(28);
    for (const auto& l : lines) {
        ui_.text(FontRole::Medium, 22, {card.x + th.dp(24), y, w - th.dp(48), th.dp(32)}, l, th.c.text, Align::Center);
        y += th.dp(32);
    }
    y += th.dp(8);
    for (const auto& l : body) {
        ui_.text(FontRole::Regular, Theme::kSmall, {card.x + th.dp(24), y, w - th.dp(48), th.dp(22)}, l, th.c.text_dim,
                 Align::Center);
        y += th.dp(22);
    }
    y += th.dp(22);
    std::string clicked;
    for (size_t i = 0; i < buttons.size(); ++i) {
        Rect b{card.x + th.dp(20), y, w - th.dp(40), bh};
        ui::Context::Press pr = ui_.press(key + ":" + buttons[i].first, b);
        bool primary = i == 0;
        Color bg = primary ? th.c.accent : th.c.surface_pressed;
        if (pr.held) bg = gfx::mix(bg, th.c.text, 0.12f);
        canvas_.fill_round_rect(b, bh / 2, bg);
        ui_.text(FontRole::Medium, Theme::kBody, b, buttons[i].second, primary ? th.c.on_accent : th.c.text,
                 Align::Center);
        if (pr.clicked) clicked = buttons[i].first;
        y += bh + gap;
    }
    return clicked;
}

// Offered once (on the first start and after updating from a version
// without GPU support) when a supported graphics card is there, and again
// as "restart?" once the download is done.
void App::update_gfx_dialog() {
    if (view_ != View::Menu || gfx_dialog_ != GfxDialog::None) return;
    gpu::GlPackage::Status st = gl_->status();
    if (st.phase == gpu::GlPackage::Phase::Done) {
        gl_->acknowledge();
        if (config_.get_str("graphics", "cpu") == "gpu" && !layers_) gfx_dialog_ = GfxDialog::Restart;
    } else if (st.phase == gpu::GlPackage::Phase::Failed) {
        gl_->acknowledge();
    }
    if (gfx_dialog_ != GfxDialog::None || config_.get_bool("graphics_offered", false) || layers_ || gl_->busy() ||
        (!gl_->available() && gl_->installed().empty()))
        return;
    for (const auto& g : gpus_)
        if (g.supported) {
            gfx_dialog_ = GfxDialog::Offer;
            dirty_ = true;
            return;
        }
}

// The choices are made: download the drivers (the restart is offered when
// they are there) or, when they are installed already, restart now.
void App::finish_gfx_setup() {
    config_.set("graphics_offered", true);
    config_.set("graphics", "gpu");
    gfx_dialog_ = GfxDialog::None;
    dirty_ = true;
    if (gl_->installed().empty()) gl_->install();
    else gfx_dialog_ = GfxDialog::Restart;
}

void App::draw_gfx_dialog() {
    if (gfx_dialog_ == GfxDialog::Offer) {
        std::string cards;
        for (const auto& g : gpus_)
            if (g.supported) cards += (cards.empty() ? "" : ", ") + g.name();
        const bool have = !gl_->installed().empty();
        std::string mb = std::to_string((gl_->size() + 500000) / 1000000);
        std::string text =
            tr("Facet found a graphics card: {}. With it, the interface and apps run smoother and the processor is "
               "freed. The processor works on every device. You can change this later in Settings > Graphics.",
               {cards});
        if (!have) text += " " + tr("The graphics card needs the OpenGL drivers, downloaded from the internet ({} MB).", {mb});
        std::string c = draw_dialog("gfxoffer", tr("How should Facet draw?"), text,
                                    {{"yes", have ? tr("Graphics card") : tr("Graphics card (download {} MB)", {mb})},
                                     {"no", tr("Processor")}});
        if (c.empty()) return;
        dirty_ = true;
        if (c == "yes") {
            gfx_dialog_ = GfxDialog::Card;
            return;
        }
        config_.set("graphics_offered", true);
        gfx_dialog_ = GfxDialog::None;
    } else if (gfx_dialog_ == GfxDialog::Card) {
        std::vector<std::pair<std::string, std::string>> buttons;
        std::string unsupported;
        for (size_t i = 0; i < gpus_.size(); ++i) {
            if (gpus_[i].supported) buttons.push_back({std::to_string(i), gpus_[i].name()});
            else unsupported += (unsupported.empty() ? "" : ", ") + gpus_[i].name() + " (" + gpus_[i].driver + ")";
        }
        buttons.push_back({"back", tr("Back")});
        std::string text = tr("Facet draws with this card. You can pick another one later in Settings > Graphics.");
        if (!unsupported.empty()) text += " " + tr("Not supported: {}.", {unsupported});
        std::string c = draw_dialog("gfxcard", tr("Which graphics card?"), text, buttons);
        if (c.empty()) return;
        dirty_ = true;
        if (c == "back") {
            gfx_dialog_ = GfxDialog::Offer;
            return;
        }
        config_.set("graphics_card", gpus_[size_t(std::stoi(c))].card);
        gfx_dialog_ = GfxDialog::Memory;
    } else if (gfx_dialog_ == GfxDialog::Memory) {
        std::string c = draw_dialog(
            "gfxvram", tr("How much video memory?"),
            tr("The most video memory Facet may use for its picture. Over it, app windows are copied by the "
               "processor."),
            {{"0", tr("No limit (recommended)")},
             {"256", tr("{} MB", {"256"})},
             {"512", tr("{} MB", {"512"})},
             {"1024", tr("{} MB", {"1024"})},
             {"back", tr("Back")}});
        if (c.empty()) return;
        dirty_ = true;
        if (c == "back") {
            gfx_dialog_ = GfxDialog::Card;
            return;
        }
        config_.set("graphics_vram_mb", std::stoi(c));
        finish_gfx_setup();
    } else if (gfx_dialog_ == GfxDialog::Restart) {
        std::string c = draw_dialog("gfxrestart", tr("OpenGL is ready"),
                                    tr("Facet restarts to draw with the graphics card. It takes a few seconds."),
                                    {{"now", tr("Restart now")}, {"later", tr("Later")}});
        if (c.empty()) return;
        gfx_dialog_ = GfxDialog::None;
        dirty_ = true;
        if (c == "now") request_restart();
        else restart_needed_ = true;
    }
}

// Transient permissions in use: a small icon in the top right corner of
// every screen; tapping it opens the module's page, where access can be stopped.
void App::draw_indicators() {
    const Theme& t = theme_;
    float W = float(canvas_.width());
    float x = W - t.dp(20);
    for (const auto& p : host_.plugins()) {
        for (const auto& perm : p->transient) {
            ui::Icon icon = perm == "camera" ? ui::Icon::Camera : perm == "microphone" ? ui::Icon::Mic : ui::Icon::Display;
            float s = t.dp(34);
            Rect r{x - s, t.dp(8), s, s};
            ui::Context::Press pr = ui_.press("ind:" + p->m.id + ":" + perm, r);
            canvas_.fill_circle(r.cx(), r.cy(), s / 2, pr.held ? t.c.surface_pressed : t.c.warn.alpha(0.25f));
            ui::draw_icon(canvas_, icon, r.inset(t.dp(7)), t.c.warn);
            if (pr.clicked) navigate(View::AppInfo, p->m.id);
            x -= s + t.dp(8);
        }
    }
}

void App::draw_permission_prompt(const plugins::PermissionPrompt& q, double t) {
    const Theme& th = theme_;
    float W = float(canvas_.width()), H = float(canvas_.height());
    canvas_.fill_rect({0, 0, W, H}, th.c.bg.alpha(0.85f));
    float w = std::min(W - th.dp(32), th.dp(560));
    const plugins::PermissionInfo* info = plugins::permission_info(q.permission);
    std::string perm = info ? tr(info->title) : q.permission;
    std::string title = tr("Allow “{}” to use: {}?", {module_name(q.plugin), perm});
    auto lines = ui_.wrap(FontRole::Medium, 22, title, w - th.dp(48));
    auto reason = q.reason.empty() ? std::vector<std::string>() : ui_.wrap(FontRole::Regular, Theme::kSmall, q.reason, w - th.dp(48));
    float bh = th.dp(56), gap = th.dp(10);
    float h = th.dp(28) + float(lines.size()) * th.dp(32) + float(reason.size()) * th.dp(22) + th.dp(22) + 3 * bh +
              2 * gap + th.dp(24);
    Rect card{(W - w) / 2, (H - h) / 2, w, h};
    canvas_.fill_round_rect(card, th.dp(Theme::kRadius + 6), th.c.surface);
    float y = card.y + th.dp(28);
    for (const auto& l : lines) {
        ui_.text(FontRole::Medium, 22, {card.x + th.dp(24), y, w - th.dp(48), th.dp(32)}, l, th.c.text, Align::Center);
        y += th.dp(32);
    }
    for (const auto& l : reason) {
        ui_.text(FontRole::Regular, Theme::kSmall, {card.x + th.dp(24), y, w - th.dp(48), th.dp(22)}, l, th.c.text_dim,
                 Align::Center);
        y += th.dp(22);
    }
    y += th.dp(22);
    struct Choice {
        const char* id;
        std::string label;
        bool allow, always;
    };
    Choice choices[] = {{"once", tr("Allow this time"), true, false},
                        {"always", tr("Allow while in use"), true, true},
                        {"deny", tr("Don't allow"), false, false}};
    for (const auto& c : choices) {
        Rect b{card.x + th.dp(20), y, w - th.dp(40), bh};
        ui::Context::Press pr = ui_.press(std::string("prompt:") + c.id, b);
        bool primary = std::string(c.id) == "once";
        Color bg = primary ? th.c.accent : th.c.surface_pressed;
        if (pr.held) bg = gfx::mix(bg, th.c.text, 0.12f);
        canvas_.fill_round_rect(b, bh / 2, bg);
        ui_.text(FontRole::Medium, Theme::kBody, b, c.label, primary ? th.c.on_accent : th.c.text, Align::Center);
        if (pr.clicked) {
            host_.answer_prompt(c.allow, c.always, t);
            dirty_ = true;
        }
        y += bh + gap;
    }
}

// Full-screen incoming call, as on a phone.
void App::draw_call(plugins::Notification& n, double t) {
    const Theme& th = theme_;
    float W = float(canvas_.width()), H = float(canvas_.height());
    canvas_.fill_rect({0, 0, W, H}, th.c.bg);
    float cy = H * 0.3f;
    float r = th.dp(56);
    canvas_.fill_circle(W / 2, cy, r, th.c.accent.alpha(0.18f));
    ui::draw_icon(canvas_, notification_icon(n), {W / 2 - r * 0.6f, cy - r * 0.6f, r * 1.2f, r * 1.2f}, th.c.accent);
    ui_.text(FontRole::Regular, Theme::kBody, {0, cy + r + th.dp(16), W, th.dp(30)}, notification_app(n), th.c.text_dim,
             Align::Center);
    ui_.text(FontRole::Medium, 34, {th.dp(24), cy + r + th.dp(48), W - th.dp(48), th.dp(48)},
             ui_.ellipsize(FontRole::Medium, 34, n.title, W - th.dp(48)), th.c.text, Align::Center);
    std::string sub = n.body.empty() ? tr("Incoming call") : n.body;
    ui_.text(FontRole::Regular, Theme::kBody, {th.dp(24), cy + r + th.dp(100), W - th.dp(48), th.dp(30)},
             ui_.ellipsize(FontRole::Regular, Theme::kBody, sub, W - th.dp(48)), th.c.text_dim, Align::Center);

    float br = th.dp(42), by = H * 0.78f, dx = std::min(W * 0.22f, th.dp(160));
    struct Button {
        const char* action;
        float x;
        Color color;
        ui::Icon icon;
        std::string label;
    };
    Button buttons[] = {{"decline", W / 2 - dx, th.c.bad, ui::Icon::Close, tr("Decline")},
                        {"accept", W / 2 + dx, th.c.good, ui::Icon::Phone, tr("Accept")}};
    uint64_t serial = n.serial;
    std::string source = n.source;
    for (const auto& b : buttons) {
        Rect hit{b.x - br, by - br, 2 * br, 2 * br};
        ui::Context::Press pr = ui_.press(std::string("call:") + b.action, hit);
        canvas_.fill_circle(b.x, by, br, pr.held ? gfx::mix(b.color, kWhite, 0.2f) : b.color);
        ui::draw_icon(canvas_, b.icon, hit.inset(br * 0.5f), kWhite);
        ui_.text(FontRole::Regular, Theme::kSmall, {b.x - th.dp(80), by + br + th.dp(10), th.dp(160), th.dp(24)}, b.label,
                 th.c.text_dim, Align::Center);
        if (pr.clicked) {
            host_.notification_action(serial, b.action, t);
            if (std::string(b.action) == "accept") open_module(source);
            dirty_ = true;
            return;
        }
    }
}

void App::draw_banner(double t) {
    const Theme& th = theme_;
    const plugins::Notification& n = *banner_;
    Rect r = banner_rect();
    ui::Context::Press pr = ui_.press("banner", r);
    canvas_.fill_round_rect(r.inset(-th.dp(1)), th.dp(Theme::kRadius + 1), th.c.divider);
    canvas_.fill_round_rect(r, th.dp(Theme::kRadius), pr.held ? th.c.surface_pressed : th.c.surface);
    float is = th.dp(28), pad = th.dp(18);
    ui::draw_icon(canvas_, notification_icon(n), {r.x + pad, r.y + pad, is, is}, th.c.accent);
    float tx = r.x + pad + is + th.dp(14), tw = r.right() - pad - tx;
    ui_.text(FontRole::Regular, Theme::kSmall, {tx, r.y + th.dp(10), tw, th.dp(22)},
             ui_.ellipsize(FontRole::Regular, Theme::kSmall, notification_app(n), tw), th.c.text_dim);
    ui_.text(FontRole::Medium, Theme::kBody, {tx, r.y + th.dp(32), tw, th.dp(26)},
             ui_.ellipsize(FontRole::Medium, Theme::kBody, n.title, tw), th.c.text);
    ui_.text(FontRole::Regular, Theme::kSmall, {tx, r.y + th.dp(58), tw, th.dp(22)},
             ui_.ellipsize(FontRole::Regular, Theme::kSmall, n.body, tw), th.c.text_dim);
    if (pr.clicked) {
        host_.notification_action(n.serial, "open", t);
        std::string source = n.source;
        banner_.reset();
        open_module(source);
        dirty_ = true;
    }
}

// ------------------------------------------------------------------ notification list

void App::draw_notifications(double t) {
    const Theme& th = theme_;
    if (ui_.begin_screen("notifications", tr("Notifications")) == ui::HeaderHit::Back) {
        navigate(View::Menu);
        ui_.end_screen();
        return;
    }
    const auto& list = host_.notifications().list();
    if (list.empty()) {
        ui_.note(tr("No notifications."));
        ui_.end_screen();
        return;
    }
    if (ui_.button("clear", tr("Clear all"))) {
        host_.notifications().clear();
        dirty_ = true;
    }
    // Copy: acting on one changes the list.
    std::vector<plugins::Notification> items(list.begin(), list.end());
    for (const auto& n : items) {
        Rect r = ui_.block(96);
        std::string key = std::to_string(n.serial);
        float xs = th.dp(48);
        Rect close{r.right() - xs - th.dp(8), r.y + th.dp(8), xs, xs};
        ui::Context::Press px = ui_.press("ndis:" + key, close);
        ui::Context::Press pr = ui_.press("nopen:" + key, {r.x, r.y, r.w - xs - th.dp(16), r.h});
        ui_.canvas().fill_round_rect(r, th.dp(Theme::kRadius), pr.held ? th.c.surface_pressed : th.c.surface);
        float is = th.dp(26), pad = th.dp(18);
        ui::draw_icon(ui_.canvas(), notification_icon(n), {r.x + pad, r.y + pad, is, is}, th.c.accent);
        float tx = r.x + pad + is + th.dp(14), tw = close.x - tx - th.dp(8);
        std::string head = notification_app(n) + " · " + ago(t - n.posted_at);
        ui_.text(FontRole::Regular, Theme::kSmall, {tx, r.y + th.dp(10), tw, th.dp(22)},
                 ui_.ellipsize(FontRole::Regular, Theme::kSmall, head, tw), th.c.text_dim);
        ui_.text(FontRole::Medium, Theme::kBody, {tx, r.y + th.dp(34), tw, th.dp(26)},
                 ui_.ellipsize(FontRole::Medium, Theme::kBody, n.title, tw), th.c.text);
        ui_.text(FontRole::Regular, Theme::kSmall, {tx, r.y + th.dp(62), tw, th.dp(22)},
                 ui_.ellipsize(FontRole::Regular, Theme::kSmall, n.body, tw), th.c.text_dim);
        if (px.held) ui_.canvas().fill_circle(close.cx(), close.cy(), xs / 2, th.c.surface_pressed);
        ui::draw_icon(ui_.canvas(), ui::Icon::Close, close.inset(th.dp(14)), th.c.text_dim);
        if (px.clicked) {
            host_.notification_action(n.serial, "dismiss", t);
            dirty_ = true;
        } else if (pr.clicked) {
            host_.notification_action(n.serial, "open", t);
            open_module(n.source);
            dirty_ = true;
            break;
        }
    }
    ui_.end_screen();
}

}  // namespace facet
