#include "ui/context.h"

#include <cmath>
#include <cstdio>

#include "gfx/utf8.h"

namespace facet::ui {

using gfx::Align;
using gfx::Color;
using gfx::Rect;

namespace {
constexpr uint64_t kBackground = 1;
constexpr float kSlopDp = 10;

uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

// ------------------------------------------------------------------ frame

void Context::begin_frame(gfx::Canvas& canvas, const Theme& theme, const Pointer& pointer, double now) {
    canvas_ = &canvas;
    theme_ = &theme;
    p_ = pointer;
    dt_ = std::clamp(now - now_, 0.001, 1.0 / 30);
    now_ = now;
    redraw_ = false;
    blocked_ = popup_.open;
    screen_ = 0;

    if (p_.pressed) {
        press_x_ = p_.x;
        press_y_ = p_.y;
        drag_ = false;
        repeated_ = false;
        hold_start_ = now;
        next_repeat_ = now + 0.45;
    }
    if ((p_.down || p_.released) && !drag_) {
        float dx = p_.x - press_x_, dy = p_.y - press_y_;
        if (dx * dx + dy * dy > theme.dp(kSlopDp) * theme.dp(kSlopDp)) drag_ = true;
    }
    canvas.clear(theme.c.bg);
}

void Context::end_frame() {
    if (popup_.open) draw_popup();
    // A press outside the focused field and the keyboard ends text input.
    if (p_.pressed && focus_.id && !focus_touched_ && !overlay_.contains(p_.x, p_.y) && !blocked_) blur();
    focus_touched_ = false;
    if (p_.pressed && active_ == 0 && !blocked_) active_ = kBackground;
    if (p_.released) active_ = 0;
}

void Context::reset_interaction() {
    active_ = 0;
    drag_ = false;
    popup_.open = false;
    blur();
}

bool Context::background_tap() const {
    return p_.released && active_ == kBackground && !drag_ && !blocked_;
}

uint64_t Context::make_id(std::string_view s) const { return fnv1a(s, screen_ ^ 0x9E3779B97F4A7C15ull) | 2; }

Color Context::tone_color(Tone tone) const {
    const Palette& c = theme_->c;
    switch (tone) {
        case Tone::Dim: return c.text_dim;
        case Tone::Good: return c.good;
        case Tone::Warn: return c.warn;
        case Tone::Bad: return c.bad;
        default: return c.text;
    }
}

Context::Press Context::interact(uint64_t id, const Rect& r) {
    Press out;
    bool in_overlay = overlay_.contains(p_.x, p_.y);
    bool hover = !blocked_ && r.contains(p_.x, p_.y) && canvas_->clip().contains(p_.x, p_.y) &&
                 in_overlay == in_overlay_;
    if (p_.pressed && hover && active_ == 0) active_ = id;
    if (active_ == id && hover && !drag_) {
        out.held = p_.down;
        out.clicked = p_.released;
    }
    return out;
}

Context::Press Context::press(std::string_view id, const Rect& r) { return interact(make_id(id), r); }

Rect Context::block(float height_dp) {
    close_group();
    Rect r{col_x_, cursor_, col_w_, theme_->dp(height_dp)};
    cursor_ += r.h + theme_->dp(Theme::kGap) * 1.5f;
    return r;
}

float Context::content_width_dp(const Theme& theme, float width_px) {
    // Mirrors begin_screen(): min(width - 2 gutters, max content), in dp.
    return std::min(width_px / theme.scale - 2 * Theme::kGutter, Theme::kMaxContent);
}

int Context::repeat_steps(uint64_t id, const Rect& r) {
    Press pr = interact(id, r);
    if (pr.held) {
        redraw_ = true;  // keep frames coming while held
        if (now_ >= next_repeat_) {
            next_repeat_ = now_ + 0.08;
            repeated_ = true;
            return 1;
        }
        return 0;
    }
    return pr.clicked && !repeated_ ? 1 : 0;
}

// ------------------------------------------------------------------ text

void Context::text(FontRole role, float size_dp, const Rect& box, std::string_view s, Color color, Align align) {
    canvas_->draw_text_in(theme_->fonts->get(role), theme_->px(size_dp), box, s, color, align);
}

float Context::text_width(FontRole role, float size_dp, std::string_view s) {
    return theme_->fonts->get(role).measure(s, theme_->px(size_dp));
}

std::string Context::ellipsize(FontRole role, float size_dp, std::string_view s, float max_w) {
    if (text_width(role, size_dp, s) <= max_w) return std::string(s);
    const char* dots = "…";
    float dots_w = text_width(role, size_dp, dots);
    gfx::Font& f = theme_->fonts->get(role);
    int px = theme_->px(size_dp);
    float w = 0;
    size_t i = 0, cut = 0;
    while (i < s.size()) {
        size_t prev = i;
        w += f.glyph(gfx::utf8_next(s, i), px).advance;
        if (w + dots_w > max_w) break;
        cut = i;
        (void)prev;
    }
    return std::string(s.substr(0, cut)) + dots;
}

std::vector<std::string> Context::wrap(FontRole role, float size_dp, std::string_view s, float max_w) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i <= s.size()) {
        size_t sp = s.find_first_of(" \n", i);
        if (sp == std::string_view::npos) sp = s.size();
        std::string_view word = s.substr(i, sp - i);
        std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && text_width(role, size_dp, candidate) > max_w) {
            lines.push_back(line);
            line = std::string(word);
        } else {
            line = candidate;
        }
        if (sp < s.size() && s[sp] == '\n') {
            lines.push_back(line);
            line.clear();
        }
        i = sp + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// ------------------------------------------------------------------ scaffold

HeaderHit Context::begin_screen(std::string_view id, std::string_view title, bool back, Icon action) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    float W = float(canvas_->width()), H = float(canvas_->height());
    screen_ = fnv1a(id);
    HeaderHit hit = HeaderHit::None;

    // Header.
    float hh = t.dp(Theme::kHeader);
    float gut = t.dp(Theme::kGutter);
    float x = gut;
    float touch = t.dp(Theme::kTouch) * 1.2f;
    if (back) {
        Rect br{gut - t.dp(8), (hh - touch) / 2, touch, touch};
        if (icon_button("__back", br, Icon::Back)) hit = HeaderHit::Back;
        x = br.right() + t.dp(4);
    }
    if (action != Icon::None) {
        Rect ar{W - gut - touch + t.dp(8), (hh - touch) / 2, touch, touch};
        if (icon_button("__action", ar, action)) hit = HeaderHit::Action;
    }
    text(FontRole::Medium, Theme::kTitle, {x, 0, W - x - gut - touch, hh}, title, c.text);

    // Scrolling content.
    float bottom = overlay_.empty() ? H : std::min(H, overlay_.y);
    viewport_ = {0, hh, W, std::max(0.f, bottom - hh)};
    Scroll& s = scroll_[screen_];
    float max_scroll = std::max(0.f, s.content_h - viewport_.h);
    if (!blocked_) {
        if (p_.pressed && viewport_.contains(p_.x, p_.y)) {
            s.tracking = true;
            s.start_offset = s.offset;
            s.velocity = 0;
        }
        if (s.tracking && (p_.down || p_.released) && drag_) {
            float target = s.start_offset - (p_.y - press_y_);
            if (target != s.offset) {
                float vmax = t.dp(4000);
                float v = std::clamp(float((target - s.offset) / dt_), -vmax, vmax);
                s.velocity = s.velocity * 0.4f + v * 0.6f;
                s.last_move = now_;
            }
            s.offset = target;
        }
        if (p_.released && s.tracking) {
            s.tracking = false;
            if (now_ - s.last_move > 0.1) s.velocity = 0;  // finger rested before lifting
        }
    }
    if (!s.tracking && std::fabs(s.velocity) > 20.f) {
        s.offset += s.velocity * float(dt_);
        s.velocity *= float(std::pow(0.04, dt_));
        redraw_ = true;
    } else if (!s.tracking) {
        s.velocity = 0;
    }
    float clamped = std::clamp(s.offset, 0.f, max_scroll);
    if (clamped != s.offset) {
        s.offset = clamped;
        if (!s.tracking) s.velocity = 0;
    }
    if (s.offset > 0.5f) canvas_->fill_rect({0, hh - 1, W, std::max(1.f, t.dp(1))}, c.divider);

    col_w_ = std::min(W - 2 * gut, t.dp(Theme::kMaxContent));
    col_x_ = std::floor((W - col_w_) / 2);
    content_top_ = viewport_.y + t.dp(4) - s.offset;
    cursor_ = content_top_;
    in_group_ = false;
    group_index_ = 0;
    canvas_->push_clip(viewport_);
    return hit;
}

void Context::end_screen() {
    close_group();
    Scroll& s = scroll_[screen_];
    float h = cursor_ - content_top_ + theme_->dp(Theme::kGutter);
    if (std::fabs(h - s.content_h) > 0.5f) {
        s.content_h = h;
        redraw_ = true;
    }
    canvas_->pop_clip();
}

void Context::open_group(std::string_view title) {
    close_group();
    const Theme& t = *theme_;
    ++group_index_;
    if (!title.empty()) {
        cursor_ += group_index_ > 1 ? t.dp(10) : 0;
        text(FontRole::Medium, Theme::kSmall, {col_x_ + t.dp(6), cursor_, col_w_, t.dp(32)}, title, t.c.text_dim);
        cursor_ += t.dp(34);
    }
    group_key_ = screen_ * 31 + uint64_t(group_index_);
    float h = group_h_[group_key_];
    if (h > 0) canvas_->fill_round_rect({col_x_, cursor_, col_w_, h}, t.dp(Theme::kRadius), t.c.surface);
    group_start_ = cursor_;
    group_rows_ = 0;
    in_group_ = true;
}

void Context::close_group() {
    if (!in_group_) return;
    in_group_ = false;
    float h = cursor_ - group_start_;
    float& cached = group_h_[group_key_];
    if (std::fabs(cached - h) > 0.5f) {
        cached = h;
        redraw_ = true;
    }
    cursor_ += theme_->dp(Theme::kGap) * 1.5f;
}

Rect Context::row(float height_dp) {
    if (!in_group_) open_group({});
    const Theme& t = *theme_;
    if (group_rows_ > 0) {
        float pad = t.dp(18);
        canvas_->fill_rect({col_x_ + pad, cursor_, col_w_ - 2 * pad, std::max(1.f, t.dp(1))}, t.c.divider);
    }
    Rect r{col_x_, cursor_, col_w_, t.dp(height_dp)};
    cursor_ += r.h;
    ++group_rows_;
    return r;
}

void Context::row_highlight(const Rect& r, bool held) {
    if (held) canvas_->fill_round_rect(r.inset(theme_->dp(4)), theme_->dp(Theme::kRadius - 4), theme_->c.surface_pressed);
}

// ------------------------------------------------------------------ widgets

void Context::section(std::string_view title) { open_group(title); }

void Context::info(std::string_view label, std::string_view value, Tone tone) {
    const Theme& t = *theme_;
    Rect r = row(Theme::kRow);
    Rect in = r.inset(t.dp(18), 0);
    float vw = std::min(text_width(FontRole::Regular, Theme::kBody, value), in.w * 0.6f);
    std::string v = ellipsize(FontRole::Regular, Theme::kBody, value, vw + 1);
    std::string l = ellipsize(FontRole::Regular, Theme::kBody, label, in.w - vw - t.dp(16));
    text(FontRole::Regular, Theme::kBody, in, l, t.c.text);
    text(FontRole::Regular, Theme::kBody, in, v, tone == Tone::Normal ? t.c.text_dim : tone_color(tone), Align::End);
}

void Context::note(std::string_view s) {
    close_group();
    const Theme& t = *theme_;
    float lh = t.dp(Theme::kSmall * 1.45f);
    for (const auto& line : wrap(FontRole::Regular, Theme::kSmall, s, col_w_ - t.dp(12))) {
        text(FontRole::Regular, Theme::kSmall, {col_x_ + t.dp(6), cursor_, col_w_, lh}, line, t.c.text_dim);
        cursor_ += lh;
    }
    cursor_ += t.dp(Theme::kGap);
}

bool Context::toggle(std::string_view id, std::string_view label, bool& value) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    Rect r = row(Theme::kRow);
    Press pr = interact(make_id(id), r);
    row_highlight(r, pr.held);
    Rect in = r.inset(t.dp(18), 0);
    float tw = t.dp(52), th = t.dp(32);
    Rect track{in.right() - tw, r.cy() - th / 2, tw, th};
    text(FontRole::Regular, Theme::kBody, {in.x, in.y, in.w - tw - t.dp(12), in.h},
         ellipsize(FontRole::Regular, Theme::kBody, label, in.w - tw - t.dp(12)), c.text);
    if (pr.clicked) value = !value;
    canvas_->fill_round_rect(track, th / 2, value ? c.accent : c.track);
    float kr = th / 2 - t.dp(4);
    float kx = value ? track.right() - th / 2 : track.x + th / 2;
    canvas_->fill_circle(kx, track.cy(), kr, value ? c.on_accent : c.text_dim);
    return pr.clicked;
}

bool Context::select(std::string_view id, std::string_view label, const std::vector<std::string>& options,
                     int& index) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    uint64_t uid = make_id(id);
    bool changed = false;
    if (popup_result_id_ == uid) {
        if (popup_result_ != index) changed = true;
        index = popup_result_;
        popup_result_id_ = 0;
    }
    Rect r = row(Theme::kRow);
    Press pr = interact(uid, r);
    row_highlight(r, pr.held);
    Rect in = r.inset(t.dp(18), 0);
    float chev = t.dp(22);
    std::string value = index >= 0 && index < int(options.size()) ? options[size_t(index)] : "—";
    float vw = std::min(text_width(FontRole::Regular, Theme::kBody, value), in.w * 0.55f);
    text(FontRole::Regular, Theme::kBody, in, ellipsize(FontRole::Regular, Theme::kBody, label, in.w - vw - chev - t.dp(20)),
         c.text);
    Rect vbox{in.x, in.y, in.w - chev - t.dp(4), in.h};
    text(FontRole::Regular, Theme::kBody, vbox, ellipsize(FontRole::Regular, Theme::kBody, value, vw + 1), c.text_dim,
         Align::End);
    draw_icon(*canvas_, Icon::Chevron, {in.right() - chev, r.cy() - chev / 2, chev, chev}, c.text_dim);

    if (pr.clicked && !options.empty()) {
        popup_.open = true;
        popup_.id = uid;
        popup_.title = std::string(label);
        popup_.options = options;
        popup_.current = index;
        popup_.outside_press = false;
        popup_.tracking = false;
        popup_.fresh = true;
    }
    return changed;
}

bool Context::round_button(uint64_t id, const Rect& r, bool plus, bool enabled, int& steps) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    int n = enabled ? repeat_steps(id, r) : 0;
    bool held = enabled && active_ == id && p_.down && !drag_;
    float rad = t.dp(20);
    canvas_->fill_circle(r.cx(), r.cy(), rad, held ? c.track : c.surface_pressed);
    Color ic = enabled ? c.text : c.text_dim.alpha(0.5f);
    float len = t.dp(7), th = t.dp(2.2f);
    canvas_->fill_rect({r.cx() - len, r.cy() - th / 2, 2 * len, th}, ic);
    if (plus) canvas_->fill_rect({r.cx() - th / 2, r.cy() - len, th, 2 * len}, ic);
    steps += plus ? n : -n;
    return n != 0;
}

bool Context::stepper(std::string_view id, std::string_view label, int& value, int min, int max, int step,
                      std::string_view unit) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    Rect r = row(Theme::kRow);
    Rect in = r.inset(t.dp(18), 0);
    auto fmt = [&](int v) {
        std::string s = std::to_string(v);
        if (!unit.empty()) s += " " + std::string(unit);
        return s;
    };
    float vw = std::max(text_width(FontRole::Medium, Theme::kBody, fmt(min)),
                        text_width(FontRole::Medium, Theme::kBody, fmt(max))) + t.dp(16);
    float bs = t.dp(52);
    Rect plus{in.right() - bs + t.dp(6), r.cy() - bs / 2, bs, bs};
    Rect val{plus.x - vw, r.y, vw, r.h};
    Rect minus{val.x - bs, r.cy() - bs / 2, bs, bs};
    text(FontRole::Regular, Theme::kBody, {in.x, in.y, minus.x - in.x - t.dp(8), in.h},
         ellipsize(FontRole::Regular, Theme::kBody, label, minus.x - in.x - t.dp(8)), c.text);

    std::string base(id);
    int steps = 0;
    round_button(make_id(base + "#-"), minus, false, value > min, steps);
    round_button(make_id(base + "#+"), plus, true, value < max, steps);
    int old = value;
    value = std::clamp(value + steps * step, min, max);
    text(FontRole::Medium, Theme::kBody, val, fmt(value), c.text, Align::Center);
    return value != old;
}

bool Context::time(std::string_view id, std::string_view label, int& minutes, int step) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    Rect r = row(Theme::kRow);
    Rect in = r.inset(t.dp(18), 0);
    float vw = text_width(FontRole::Medium, Theme::kBody, "00:00") + t.dp(20);
    float bs = t.dp(52);
    Rect plus{in.right() - bs + t.dp(6), r.cy() - bs / 2, bs, bs};
    Rect val{plus.x - vw, r.y, vw, r.h};
    Rect minus{val.x - bs, r.cy() - bs / 2, bs, bs};
    text(FontRole::Regular, Theme::kBody, {in.x, in.y, minus.x - in.x - t.dp(8), in.h},
         ellipsize(FontRole::Regular, Theme::kBody, label, minus.x - in.x - t.dp(8)), c.text);

    std::string base(id);
    int steps = 0;
    round_button(make_id(base + "#-"), minus, false, true, steps);
    round_button(make_id(base + "#+"), plus, true, true, steps);
    int old = minutes;
    minutes = ((minutes + steps * step) % 1440 + 1440) % 1440;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
    text(FontRole::Medium, Theme::kBody, val, buf, c.text, Align::Center);
    return minutes != old;
}

void Context::level(std::string_view label, float value, std::string_view s) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    Rect r = row(Theme::kRow);
    Rect in = r.inset(t.dp(18), 0);
    float bw = std::min(in.w * 0.4f, t.dp(260)), bh = t.dp(8);
    Rect bar{in.right() - bw, r.cy() - bh / 2, bw, bh};
    float tw = s.empty() ? 0 : text_width(FontRole::Regular, Theme::kSmall, s) + t.dp(12);
    text(FontRole::Regular, Theme::kBody, {in.x, in.y, in.w - bw - tw - t.dp(12), in.h}, label, c.text);
    if (!s.empty()) text(FontRole::Regular, Theme::kSmall, {bar.x - tw, r.y, tw - t.dp(12), r.h}, s, c.text_dim, Align::End);
    canvas_->fill_round_rect(bar, bh / 2, c.track);
    float v = std::clamp(value, 0.f, 1.f);
    if (v > 0) canvas_->fill_round_rect({bar.x, bar.y, std::max(bh, bw * v), bh}, bh / 2, c.accent);
}

bool Context::button(std::string_view id, std::string_view label, ButtonStyle style) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    if (style == ButtonStyle::Primary) {
        close_group();
        Rect r{col_x_, cursor_, col_w_, t.dp(Theme::kRow)};
        cursor_ += r.h + t.dp(Theme::kGap) * 1.5f;
        Press pr = interact(make_id(id), r);
        canvas_->fill_round_rect(r, t.dp(Theme::kRadius), pr.held ? mix(c.accent, c.bg, 0.2f) : c.accent);
        text(FontRole::Medium, Theme::kBody, r, label, c.on_accent, Align::Center);
        return pr.clicked;
    }
    Rect r = row(Theme::kRow);
    Press pr = interact(make_id(id), r);
    row_highlight(r, pr.held);
    text(FontRole::Medium, Theme::kBody, r, label, style == ButtonStyle::Danger ? c.bad : c.accent, Align::Center);
    return pr.clicked;
}

// ------------------------------------------------------------------ text input

namespace {
size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
void utf8_pop(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}
}  // namespace

void Context::blur() {
    focus_ = {};
    edits_.clear();
    ensure_visible_ = false;
}

void Context::push_edit(EditKind kind, std::string text) {
    if (focus_.id) edits_.emplace_back(kind, std::move(text));
}

Context::TextResult Context::text_field(std::string_view id, std::string_view label, std::string& value,
                                        const TextOptions& o) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    uint64_t uid = make_id(id);
    TextResult res;

    if (focus_.id == uid && !edits_.empty()) {
        for (auto& [kind, text] : edits_) {
            if (kind == EditKind::Insert) {
                if (o.mode == InputMode::Number && text.find_first_not_of("0123456789,.-") != std::string::npos) continue;
                if (utf8_length(value) + utf8_length(text) > o.max_length) continue;
                value += text;
                res.changed = true;
            } else if (kind == EditKind::Backspace) {
                if (!value.empty()) utf8_pop(value), res.changed = true;
            } else {
                res.submitted = true;
            }
        }
        edits_.clear();
        if (res.submitted) blur();
    }

    Rect r = row(Theme::kRow);
    Press pr = interact(uid, r);
    if (p_.pressed && active_ == uid) focus_touched_ = true;
    if (pr.clicked && focus_.id != uid) {
        focus_ = {uid, o.secure, o.mode};
        edits_.clear();
        ensure_visible_ = true;
        redraw_ = true;
    }
    bool focused = focus_.id == uid;
    row_highlight(r, pr.held);

    Rect in = r.inset(t.dp(18), 0);
    float label_w = std::min(text_width(FontRole::Regular, Theme::kBody, label), in.w * 0.45f);
    text(FontRole::Regular, Theme::kBody, in, ellipsize(FontRole::Regular, Theme::kBody, label, label_w + 1),
         focused ? c.accent : c.text);

    // Value, right-aligned; long values show their end, which is being typed.
    Rect vbox{in.x + label_w + t.dp(16), in.y, in.w - label_w - t.dp(16), in.h};
    std::string shown;
    if (o.secure) {
        for (size_t i = 0, n = utf8_length(value); i < n; ++i) shown += "•";
    } else {
        shown = value;
    }
    float caret_w = focused ? t.dp(2) + t.dp(3) : 0;
    float max_w = vbox.w - caret_w;
    if (text_width(FontRole::Regular, Theme::kBody, shown) > max_w) {
        std::string rest = shown;
        const std::string dots = "…";
        while (!rest.empty() && text_width(FontRole::Regular, Theme::kBody, dots + rest) > max_w) {
            size_t cut = 1;  // drop one UTF-8 character from the front
            while (cut < rest.size() && (static_cast<unsigned char>(rest[cut]) & 0xC0) == 0x80) ++cut;
            rest.erase(0, cut);
        }
        shown = dots + rest;
    }

    Rect text_box{vbox.x, vbox.y, vbox.w - caret_w, vbox.h};
    if (value.empty() && !focused)
        text(FontRole::Regular, Theme::kBody, text_box, ellipsize(FontRole::Regular, Theme::kBody, o.placeholder, max_w),
             c.text_dim, gfx::Align::End);
    else
        text(FontRole::Regular, Theme::kBody, text_box, shown, c.text, gfx::Align::End);
    if (focused) {
        float ch = t.dp(26);
        canvas_->fill_rect({vbox.right() - t.dp(2), r.cy() - ch / 2, t.dp(2), ch}, c.accent);
        canvas_->fill_rect({in.x, r.bottom() - t.dp(2), in.w, t.dp(2)}, c.accent);
    }

    // Keep the focused field above the keyboard.
    if (focused && ensure_visible_ && !overlay_.empty()) {
        float over = r.bottom() + t.dp(12) - viewport_.bottom();
        if (over > 0) {
            scroll_[screen_].offset += over;
            redraw_ = true;
        }
        ensure_visible_ = false;
    }
    return res;
}

// ------------------------------------------------------------------ free layout

bool Context::tile(std::string_view id, const Rect& r, std::string_view title, std::string_view subtitle, Icon icon,
                   Tone tone) {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    Press pr = interact(make_id(id), r);
    canvas_->fill_round_rect(r, t.dp(Theme::kRadius + 6), pr.held ? c.surface_pressed : c.surface);
    float pad = t.dp(22);
    Color ic = tone == Tone::Normal ? c.accent : tone_color(tone);
    float cr = t.dp(30);
    canvas_->fill_circle(r.x + pad + cr, r.y + pad + cr, cr, ic.alpha(0.16f));
    float is = t.dp(32);
    draw_icon(*canvas_, icon, {r.x + pad + cr - is / 2, r.y + pad + cr - is / 2, is, is}, ic);

    float inner = r.w - 2 * pad;
    float sub_h = subtitle.empty() ? 0 : t.dp(24);
    Rect title_box{r.x + pad, r.bottom() - pad - sub_h - t.dp(34), inner, t.dp(34)};
    text(FontRole::Medium, 23, title_box, ellipsize(FontRole::Medium, 23, title, inner), c.text);
    if (!subtitle.empty())
        text(FontRole::Regular, Theme::kSmall, {r.x + pad, r.bottom() - pad - sub_h, inner, sub_h},
             ellipsize(FontRole::Regular, Theme::kSmall, subtitle, inner), tone == Tone::Normal ? c.text_dim : ic);
    return pr.clicked;
}

bool Context::icon_button(std::string_view id, const Rect& r, Icon icon) {
    const Theme& t = *theme_;
    Press pr = interact(make_id(id), r);
    if (pr.held) canvas_->fill_circle(r.cx(), r.cy(), std::min(r.w, r.h) * 0.45f, t.c.surface_pressed);
    float is = t.dp(28);
    draw_icon(*canvas_, icon, {r.cx() - is / 2, r.cy() - is / 2, is, is}, t.c.text);
    return pr.clicked;
}

// ------------------------------------------------------------------ popup

void Context::draw_popup() {
    const Theme& t = *theme_;
    const Palette& c = t.c;
    float W = float(canvas_->width()), H = float(canvas_->height());
    bool was_blocked = blocked_;
    blocked_ = false;
    uint64_t saved_screen = screen_;
    screen_ = popup_.id;

    canvas_->fill_rect({0, 0, W, H}, c.scrim);
    float rh = t.dp(Theme::kRow), th = t.dp(64);
    size_t total = popup_.options.size();
    size_t max_rows = size_t(std::max(1.f, (H * 0.85f - th - t.dp(12)) / rh));
    size_t n = std::min(total, max_rows);
    float cw = std::min(W - 2 * t.dp(Theme::kGutter), t.dp(480));
    float ch = th + rh * float(n) + t.dp(12);
    Rect card{std::floor((W - cw) / 2), std::floor((H - ch) / 2), cw, ch};
    canvas_->fill_round_rect(card, t.dp(Theme::kRadius + 6), c.surface);
    text(FontRole::Medium, 21, {card.x + t.dp(24), card.y, card.w - t.dp(48), th}, popup_.title, c.text);

    // Scrolling list.
    Rect list{card.x, card.y + th, card.w, rh * float(n)};
    float max_scroll = std::max(0.f, rh * float(total) - list.h);
    if (popup_.fresh) {
        popup_.scroll = rh * float(std::max(0, popup_.current)) - (list.h - rh) / 2;
        popup_.fresh = false;
    }
    if (p_.pressed && list.contains(p_.x, p_.y)) {
        popup_.tracking = true;
        popup_.scroll_start = popup_.scroll;
    }
    if (popup_.tracking && (p_.down || p_.released) && drag_) popup_.scroll = popup_.scroll_start - (p_.y - press_y_);
    if (p_.released) popup_.tracking = false;
    popup_.scroll = std::clamp(popup_.scroll, 0.f, max_scroll);

    int chosen = -1;
    canvas_->push_clip(list);
    size_t first = size_t(popup_.scroll / rh);
    for (size_t i = first; i < total && i <= first + n; ++i) {
        Rect r{card.x, list.y + rh * float(i) - popup_.scroll, card.w, rh};
        Press pr = interact(make_id("opt" + std::to_string(i)), r);
        if (pr.held) canvas_->fill_rect(r.inset(t.dp(8), t.dp(3)), c.surface_pressed);
        bool cur = int(i) == popup_.current;
        Rect in = r.inset(t.dp(24), 0);
        text(cur ? FontRole::Medium : FontRole::Regular, Theme::kBody, in,
             ellipsize(FontRole::Regular, Theme::kBody, popup_.options[i], in.w - t.dp(30)), cur ? c.accent : c.text);
        if (cur) canvas_->fill_circle(in.right() - t.dp(6), r.cy(), t.dp(5), c.accent);
        if (pr.clicked) chosen = int(i);
    }
    canvas_->pop_clip();
    if (max_scroll > 0) {  // scroll indicator
        float bar_h = std::max(t.dp(24), list.h * list.h / (rh * float(total)));
        float bar_y = list.y + (list.h - bar_h) * popup_.scroll / max_scroll;
        canvas_->fill_round_rect({card.right() - t.dp(8), bar_y, t.dp(3), bar_h}, t.dp(1.5f), c.track);
    }

    if (p_.pressed && !card.contains(p_.x, p_.y)) popup_.outside_press = true;
    bool dismiss = p_.released && popup_.outside_press && !card.contains(p_.x, p_.y);
    if (p_.released) popup_.outside_press = false;

    if (chosen >= 0) {
        popup_result_id_ = popup_.id;
        popup_result_ = chosen;
        popup_.open = false;
        redraw_ = true;
    } else if (dismiss) {
        popup_.open = false;
        redraw_ = true;
    }
    screen_ = saved_screen;
    blocked_ = was_blocked;
}

}  // namespace facet::ui
