#include "ui/skin.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "ui/canvas_ops.h"

namespace facet::ui {

namespace {

const char* const kCoreTokens[] = {"bg",   "surface", "surface_pressed", "text", "text_dim", "accent", "on_accent",
                                   "divider", "track", "good",           "warn", "bad",      "scrim"};

bool is_core_token(const std::string& name) {
    for (const char* t : kCoreTokens)
        if (name == t) return true;
    return false;
}

// Colour names of a pack's own: letters, digits, - and _.
bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

bool is_hex_color(const std::string& v) {
    if (v.empty() || v[0] != '#' || (v.size() != 7 && v.size() != 9)) return false;
    return std::all_of(v.begin() + 1, v.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// An array of operation objects; false if it is something else or too long.
bool take_ops(const Json& in, Json& out, bool& present, size_t& total, std::string& error, const char* where) {
    if (in.is_null()) return true;
    if (!in.is_array()) {
        error = std::string(where) + ": a list of drawing operations is expected";
        return false;
    }
    if (in.size() > Skin::kMaxOpsPerHook) {
        error = std::string(where) + ": too many operations";
        return false;
    }
    for (const auto& op : in.items())
        if (!op.is_object() || !op["op"].is_string()) {
            error = std::string(where) + ": every operation needs an \"op\"";
            return false;
        }
    total += in.size();
    out = in;
    present = in.size() > 0;
    return true;
}

bool take_hook(const Json& in, Skin::Hook& out, size_t& total, std::string& error, const char* where) {
    if (in.is_null()) return true;
    if (!in.is_object()) {
        error = std::string(where) + ": an object with \"under\" and \"over\" is expected";
        return false;
    }
    return take_ops(in["under"], out.under, out.has_under, total, error, where) &&
           take_ops(in["over"], out.over, out.has_over, total, error, where);
}

}  // namespace

std::shared_ptr<Skin> Skin::load(const std::string& path, std::string& error) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "no such file";
        return nullptr;
    }
    if (size_t(st.st_size) > kMaxFileBytes) {
        error = "the theme file is too big";
        return nullptr;
    }
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Json j;
    std::string perr;
    if (!Json::parse(text, j, &perr)) {
        error = "not valid JSON: " + perr;
        return nullptr;
    }
    return parse(j, error);
}

std::shared_ptr<Skin> Skin::parse(const Json& j, std::string& error) {
    if (!j.is_object()) {
        error = "a JSON object is expected";
        return nullptr;
    }
    if (j["format"].is_number() && j["format"].as_int() != 1) {
        error = "a newer theme format than this Facet reads";
        return nullptr;
    }
    auto s = std::make_shared<Skin>();
    s->dark = j["base"].as_string("dark") != "light";
    s->animated = j["animated"].as_bool();
    s->radius = std::clamp(float(j["radius"].as_number(0)), 0.f, 60.f);

    static const Theme probe = Theme::make(true, 1.f, nullptr);  // for colour parsing
    for (const auto& [name, value] : j["palette"].fields()) {
        if (!value.is_string() || !is_hex_color(value.str())) {
            error = "colour \"" + name + "\" must be \"#RRGGBB\" or \"#RRGGBBAA\"";
            return nullptr;
        }
        gfx::Color c = color_from(probe, value.str(), gfx::Color{});
        if (is_core_token(name)) s->core_[name] = c;
        else if (valid_name(name)) s->custom[name] = c;
        else {
            error = "bad colour name \"" + name + "\"";
            return nullptr;
        }
    }

    size_t total = 0;
    for (const auto& [id, hook] : j["screens"].fields()) {
        if (!valid_name(id) && id != "*") {
            error = "bad screen name \"" + id + "\"";
            return nullptr;
        }
        Hook h;
        if (!take_hook(hook, h, total, error, ("screens/" + id).c_str())) return nullptr;
        if (id == "*") s->everywhere = std::move(h);
        else s->screens_[id] = std::move(h);
    }
    if (!take_hook(j["tile"], s->tile, total, error, "tile") || !take_hook(j["card"], s->card, total, error, "card"))
        return nullptr;
    if (total > kMaxOpsTotal) {
        error = "too many drawing operations";
        return nullptr;
    }
    return s;
}

void Skin::apply(Palette& p) const {
    auto set = [&](const char* name, gfx::Color& c) {
        auto it = core_.find(name);
        if (it != core_.end()) c = it->second;
    };
    set("bg", p.bg);
    set("surface", p.surface);
    set("surface_pressed", p.surface_pressed);
    set("text", p.text);
    set("text_dim", p.text_dim);
    set("accent", p.accent);
    set("on_accent", p.on_accent);
    set("divider", p.divider);
    set("track", p.track);
    set("good", p.good);
    set("warn", p.warn);
    set("bad", p.bad);
    set("scrim", p.scrim);
}

const Skin::Hook* Skin::screen(const std::string& id) const {
    auto it = screens_.find(id);
    return it == screens_.end() ? nullptr : &it->second;
}

}  // namespace facet::ui
