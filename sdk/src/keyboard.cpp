#include "facet/keyboard.h"

#include <algorithm>
#include <cctype>

#include "i18n/keyboard_layouts.h"

namespace facet::sdk {

namespace {

std::vector<std::string> split_utf8(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// Upper case for ASCII and Cyrillic; other characters are returned as is.
std::string upper(const std::string& ch) {
    if (ch.size() == 1) return std::string(1, char(std::toupper(static_cast<unsigned char>(ch[0]))));
    if (ch.size() != 2) return ch;
    unsigned cp = (unsigned(static_cast<unsigned char>(ch[0])) & 0x1F) << 6 | (static_cast<unsigned char>(ch[1]) & 0x3F);
    if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;       // Cyrillic small a..ya -> capitals
    else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;  // Cyrillic small io etc. -> capitals
    else return ch;
    return {char(0xC0 | (cp >> 6)), char(0x80 | (cp & 0x3F))};
}

constexpr float kPad = 8, kGap = 6, kKeyH = 52, kMaxKeyW = 84, kRadius = 10, kFont = 22;

// Character key: id "k:<char>", the label shows the shifted form.
void char_key(Canvas& c, float x, float y, float w, const std::string& ch, bool shift) {
    std::string shown = shift ? upper(ch) : ch;
    c.rrect(x, y, w, kKeyH, kRadius, "surface", "k:" + ch, "surface_pressed");
    c.text(x, y, w, kKeyH, shown, kFont, "text", "center");
}

// Function key with a text label or an icon.
void fn_key(Canvas& c, float x, float y, float w, const std::string& id, const std::string& label,
            const std::string& icon = {}, bool accent = false) {
    c.rrect(x, y, w, kKeyH, kRadius, accent ? "accent" : "track", id, accent ? "" : "surface_pressed");
    if (!icon.empty()) c.icon(x + w / 2 - 13, y + kKeyH / 2 - 13, 26, icon, accent ? "on_accent" : "text");
    else c.text(x, y, w, kKeyH, label, 17, accent ? "on_accent" : "text", "center", "medium");
}

}  // namespace

void Keyboard::configure(Mode mode, std::vector<std::string> langs) {
    mode_ = mode;
    if (!langs.empty()) langs_ = std::move(langs);
    lang_ = 0;
    symbols_ = false;
    shift_ = false;
}

std::vector<std::vector<std::string>> Keyboard::letter_rows() const {
    std::vector<std::vector<std::string>> rows;
    const char* const* src = symbols_ ? layouts::symbol_rows() : layouts::for_lang(langs_[lang_ % langs_.size()]).rows;
    for (int i = 0; i < 3; ++i) rows.push_back(split_utf8(src[i]));
    return rows;
}

Canvas Keyboard::build(float width, float& height) const {
    return mode_ == Mode::Number ? build_number(width, height) : build_text(width, height);
}

Canvas Keyboard::build_text(float width, float& height) const {
    Canvas c;
    auto rows = letter_rows();
    size_t widest = 0;
    for (const auto& r : rows) widest = std::max(widest, r.size());
    widest = std::max<size_t>(widest, 10);
    float kw = std::min(kMaxKeyW, (width - 2 * kPad - kGap * float(widest - 1)) / float(widest));
    float y = kPad;

    // Rows 1-2: characters, centred.
    for (int r = 0; r < 2; ++r) {
        float total = float(rows[r].size()) * kw + kGap * float(rows[r].size() - 1);
        float x = (width - total) / 2;
        for (const auto& ch : rows[r]) {
            char_key(c, x, y, kw, ch, shift_ && !symbols_);
            x += kw + kGap;
        }
        y += kKeyH + kGap;
    }
    // Row 3: shift, characters, backspace.
    {
        float side = kw * 1.5f;
        float total = float(rows[2].size()) * kw + 2 * side + kGap * float(rows[2].size() + 1);
        float x = (width - total) / 2;
        if (symbols_) c.rrect(x, y, side, kKeyH, kRadius, "track");  // no shift for symbols
        else fn_key(c, x, y, side, "shift", {}, "shift", shift_);
        x += side + kGap;
        for (const auto& ch : rows[2]) {
            char_key(c, x, y, kw, ch, shift_ && !symbols_);
            x += kw + kGap;
        }
        fn_key(c, x, y, side, "back", {}, "backspace");
        y += kKeyH + kGap;
    }
    // Row 4: layer switch, language, space, done.
    {
        float total = std::min(width - 2 * kPad, kw * 10 + kGap * 9);
        float x = (width - total) / 2;
        float layer_w = kw * 1.5f, lang_w = langs_.size() > 1 ? kw * 1.2f : 0, done_w = kw * 2;
        float space_w = total - layer_w - done_w - (lang_w > 0 ? lang_w + kGap : 0) - 2 * kGap;
        fn_key(c, x, y, layer_w, "layer", symbols_ ? labels_.letters : labels_.symbols);
        x += layer_w + kGap;
        if (lang_w > 0) {
            fn_key(c, x, y, lang_w, "lang", layouts::for_lang(langs_[lang_ % langs_.size()]).label);
            x += lang_w + kGap;
        }
        c.rrect(x, y, space_w, kKeyH, kRadius, "surface", "space", "surface_pressed");
        c.text(x, y, space_w, kKeyH, labels_.space, 15, "text_dim", "center");
        x += space_w + kGap;
        fn_key(c, x, y, done_w, "enter", labels_.done, {}, true);
        y += kKeyH;
    }
    height = y + kPad;
    return c;
}

Canvas Keyboard::build_number(float width, float& height) const {
    Canvas c;
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ",", "0", "back"};
    float kw = std::min(120.f, (width - 2 * kPad - 3 * kGap) / 4);
    float total = 4 * kw + 3 * kGap;
    float x0 = (width - total) / 2, y = kPad;
    for (int i = 0; i < 12; ++i) {
        float x = x0 + float(i % 3) * (kw + kGap), ky = y + float(i / 3) * (kKeyH + kGap);
        std::string k = keys[i];
        if (k == "back") fn_key(c, x, ky, kw, "back", {}, "backspace");
        else char_key(c, x, ky, kw, k, false);
    }
    // Right column: done spans the full height.
    float dx = x0 + 3 * (kw + kGap), dh = 4 * kKeyH + 3 * kGap;
    c.rrect(dx, y, kw, dh, kRadius, "accent", "enter");
    c.text(dx, y, kw, dh, labels_.done, 17, "on_accent", "center", "medium");
    height = y + dh + kPad;
    return c;
}

KeyAction Keyboard::press(const std::string& hit) {
    KeyAction a;
    if (hit.rfind("k:", 0) == 0) {
        a.kind = KeyAction::Kind::Insert;
        a.text = shift_ && !symbols_ ? upper(hit.substr(2)) : hit.substr(2);
        shift_ = false;  // one-shot shift
    } else if (hit == "space") {
        a.kind = KeyAction::Kind::Insert;
        a.text = " ";
    } else if (hit == "back") {
        a.kind = KeyAction::Kind::Backspace;
    } else if (hit == "enter") {
        a.kind = KeyAction::Kind::Enter;
    } else if (hit == "hide") {
        a.kind = KeyAction::Kind::Hide;
    } else if (hit == "shift") {
        shift_ = !shift_;
    } else if (hit == "layer") {
        symbols_ = !symbols_;
        shift_ = false;
    } else if (hit == "lang") {
        lang_ = (lang_ + 1) % langs_.size();
        shift_ = false;
    }
    return a;
}

}  // namespace facet::sdk
