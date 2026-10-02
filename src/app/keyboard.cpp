// On-screen keyboard: shows the selected keyboard plugin, or the built-in one,
// at the bottom of the screen while a text field has focus.
//
// Secure fields (PINs, passwords) always use the built-in keyboard, so a
// third-party keyboard never learns what is typed into them. Keyboard plugins
// are never told the contents of any field; they only send the keys they draw.
#include "app/app.h"
#include "i18n/i18n.h"
#include "ui/canvas_ops.h"

namespace facet {

std::string App::keyboard_plugin() const {
    std::string id = config_.get_str("keyboard", "keyboard");  // the bundled plugin by default
    if (id == "builtin") return {};
    for (const auto& p : host_.plugins())
        if (p->m.id == id && p->state == plugins::State::Running && p->m.provides_cap("input.keyboard")) return id;
    return {};
}

std::vector<std::string> App::keyboard_langs() const {
    const std::string& ui = catalog().language();
    if (ui == "en") return {"en"};
    return {ui, "en"};
}

void App::draw_keyboard() {
    // Who receives the keys: the focused text field, or the owner of a
    // surface on screen (the open plugin's own, or one lent to it) while that
    // owner asks for text input.
    std::string surface_owner;
    if (!ui_.has_focus() && view_ == View::Plugin) {
        for (const auto& id : surface_owners_) {
            const plugins::Plugin* p = host_.find(id);
            if (p && p->state == plugins::State::Running && p->text_input) {
                surface_owner = p->m.id;
                break;
            }
        }
    }
    if (!ui_.has_focus() && surface_owner.empty()) {
        if (kb_.visible) close_keyboard();
        return;
    }
    const bool number = surface_owner.empty() ? ui_.focus().mode == ui::InputMode::Number
                                              : host_.find(surface_owner)->text_mode == "number";
    const bool secure = surface_owner.empty() && ui_.focus().secure;
    const uint64_t field = surface_owner.empty() ? ui_.focus().id
                                                 : (uint64_t(1) << 63) | std::hash<std::string>{}(surface_owner);
    const std::string want = secure ? std::string() : keyboard_plugin();
    const float W = float(canvas_.width()), H = float(canvas_.height());
    const float width_dp = W / theme_.scale;

    if (!kb_.visible || kb_.field != field || kb_.plugin != want) {
        if (kb_.visible && !kb_.plugin.empty() && kb_.plugin != want) host_.keyboard_hide(kb_.plugin);
        auto mode = number ? sdk::Keyboard::Mode::Number : sdk::Keyboard::Mode::Text;
        builtin_kb_.configure(mode, keyboard_langs());
        builtin_kb_.set_labels({tr("Done"), tr("space"), "?123", "ABC"});
        if (!want.empty()) host_.keyboard_show(want, number ? "number" : "text", width_dp, keyboard_langs());
        kb_ = {true, field, want, surface_owner};
    }

    // The plugin's drawing once it has answered; the built-in one until then.
    const bool from_plugin = !kb_.plugin.empty() && host_.keyboard_ready(kb_.plugin);
    Json ops;
    float h = 0;
    if (from_plugin) {
        const plugins::Plugin* p = host_.find(kb_.plugin);
        ops = p->keyboard_ops;
        h = p->keyboard_height;
    } else {
        ops = builtin_kb_.build(width_dp, h).ops();
    }
    h = std::min(h, float(H) / theme_.scale * 0.6f);  // never cover most of the screen

    gfx::Rect r{0, H - theme_.dp(h), W, theme_.dp(h)};
    ui_.set_overlay(r);
    ui_.begin_overlay();
    canvas_.fill_rect(r, theme_.c.bg);
    canvas_.fill_rect({0, r.y, W, std::max(1.f, theme_.dp(1))}, theme_.c.divider);
    std::string hit = ui::draw_ops(ui_, "__keyboard", ops, r);
    ui_.end_overlay();

    if (!hit.empty()) {
        if (from_plugin) {
            host_.keyboard_key(kb_.plugin, hit);
        } else {
            sdk::KeyAction a = builtin_kb_.press(hit);
            using K = sdk::KeyAction::Kind;
            if (a.kind == K::Insert) kb_pending_.emplace_back("insert", a.text);
            else if (a.kind == K::Backspace) kb_pending_.emplace_back("backspace", "");
            else if (a.kind == K::Enter) kb_pending_.emplace_back("enter", "");
            else if (a.kind == K::Hide) kb_pending_.emplace_back("hide", "");
            dirty_ = true;  // redraw: layout (shift, symbols) may have changed
        }
    }
    if (r.y != kb_rect_.y || r.h != kb_rect_.h) {
        kb_rect_ = r;
        dirty_ = true;  // relayout the screen above the keyboard
    }
}

void App::close_keyboard() {
    if (!kb_.plugin.empty()) host_.keyboard_hide(kb_.plugin);
    kb_ = {};
    kb_rect_ = {};
    dirty_ = true;
}

void App::apply_key(const std::string& action, const std::string& text) {
    if (!kb_.surface_owner.empty()) {
        host_.send_text(kb_.surface_owner, action, text);  // the plugin decides when to stop
        return;
    }
    using E = ui::Context::EditKind;
    if (action == "insert") ui_.push_edit(E::Insert, text);
    else if (action == "backspace") ui_.push_edit(E::Backspace);
    else if (action == "enter") ui_.push_edit(E::Enter);
    else if (action == "hide") ui_.blur();
}

// Keys from the keyboard plugin: accepted only from the plugin that currently
// serves the focused field, and never for secure fields.
void App::take_plugin_input() {
    for (const auto& a : host_.take_input()) {
        if (!kb_.visible || a.plugin != kb_.plugin) continue;
        if (kb_.surface_owner.empty() && (!ui_.has_focus() || ui_.focus().secure)) continue;
        apply_key(a.action, a.text);
        dirty_ = true;
    }
}

}  // namespace facet
