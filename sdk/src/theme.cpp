#include "facet/theme.h"

#include <cmath>

namespace facet::sdk {

namespace {

const char* wave_name(Wave w) {
    switch (w) {
        case Wave::Sine: return "sin";
        case Wave::Triangle: return "tri";
        case Wave::Saw: return "saw";
        case Wave::Pulse: return "pulse";
        case Wave::Flicker: return "flicker";
    }
    return "sin";
}

// 0.01 dp is plenty and keeps the file small.
double round2(double v) { return std::round(v * 100) / 100; }
double round3(double v) { return std::round(v * 1000) / 1000; }

}  // namespace

// ---------------------------------------------------------------- Num

Num Num::width(double fraction, double add) {
    Num n(add);
    n.w_ = fraction;
    return n;
}

Num Num::height(double fraction, double add) {
    Num n(add);
    n.h_ = fraction;
    return n;
}

Num Num::wave(Wave kind, double min, double max, double period_s, double phase) {
    Num n(0.0);
    n.wave_ = true;
    n.kind_ = kind;
    n.min_ = min;
    n.max_ = max;
    n.period_ = period_s;
    n.phase_ = phase;
    return n;
}

Num Num::operator+(const Num& o) const {
    Num n = *this;
    n.w_ += o.w_;
    n.h_ += o.h_;
    n.k_ += o.k_;
    if (o.wave_ && !n.wave_) {  // a second wave is dropped: use another shape for it
        n.wave_ = true;
        n.kind_ = o.kind_;
        n.min_ = o.min_;
        n.max_ = o.max_;
        n.period_ = o.period_;
        n.phase_ = o.phase_;
    }
    return n;
}

Num Num::operator-(const Num& o) const {
    Num neg = o;
    neg.w_ = -neg.w_;
    neg.h_ = -neg.h_;
    neg.k_ = -neg.k_;
    if (neg.wave_) {
        neg.min_ = -o.max_;
        neg.max_ = -o.min_;
    }
    return *this + neg;
}

Num Num::operator*(double f) const {
    Num n = *this;
    n.w_ *= f;
    n.h_ *= f;
    n.k_ *= f;
    n.min_ *= f;
    n.max_ *= f;
    return n;
}

Json Num::to_json() const {
    if (w_ == 0 && h_ == 0 && !wave_) return Json(round2(k_));
    Json j = Json::object();
    if (w_ != 0) j["w"] = round3(w_);
    if (h_ != 0) j["h"] = round3(h_);
    if (k_ != 0) j["k"] = round2(k_);
    if (wave_) {
        j["wave"] = wave_name(kind_);
        j["min"] = round3(min_);
        j["max"] = round3(max_);
        j["period"] = round2(period_);
        if (phase_ != 0) j["phase"] = round3(phase_);
    }
    return j;
}

// ---------------------------------------------------------------- Scene

Json& Scene::add(const char* op) {
    Json item = Json::object();
    item["op"] = op;
    ops_.push_back(std::move(item));
    return ops_.back();
}

Scene& Scene::rect(Num x, Num y, Num w, Num h, std::string color) {
    Json& j = add("rect");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["w"] = w.to_json(), j["h"] = h.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::rrect(Num x, Num y, Num w, Num h, Num radius, std::string color) {
    Json& j = add("rrect");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["w"] = w.to_json(), j["h"] = h.to_json();
    j["r"] = radius.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::circle(Num cx, Num cy, Num r, std::string color) {
    Json& j = add("circle");
    j["cx"] = cx.to_json(), j["cy"] = cy.to_json(), j["r"] = r.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::ellipse(Num cx, Num cy, Num rx, Num ry, std::string color) {
    Json& j = add("ellipse");
    j["cx"] = cx.to_json(), j["cy"] = cy.to_json(), j["rx"] = rx.to_json(), j["ry"] = ry.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::ring(Num cx, Num cy, Num r, Num width, std::string color) {
    Json& j = add("ring");
    j["cx"] = cx.to_json(), j["cy"] = cy.to_json(), j["r"] = r.to_json(), j["width"] = width.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::line(Num x1, Num y1, Num x2, Num y2, Num width, std::string color) {
    Json& j = add("line");
    j["x1"] = x1.to_json(), j["y1"] = y1.to_json(), j["x2"] = x2.to_json(), j["y2"] = y2.to_json();
    j["width"] = width.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::arc(Num cx, Num cy, Num r, Num width, Num start, Num sweep, std::string color) {
    Json& j = add("arc");
    j["cx"] = cx.to_json(), j["cy"] = cy.to_json(), j["r"] = r.to_json(), j["width"] = width.to_json();
    j["start"] = start.to_json(), j["sweep"] = sweep.to_json();
    j["color"] = std::move(color);
    return *this;
}

namespace {
Json points_json(const std::vector<Num>& points) {
    Json pts = Json::array();
    for (const Num& v : points) pts.push_back(v.to_json());
    return pts;
}
}  // namespace

Scene& Scene::poly(std::vector<Num> points, std::string color) {
    Json& j = add("poly");
    j["pts"] = points_json(points);
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::polyline(std::vector<Num> points, Num width, std::string color) {
    Json& j = add("polyline");
    j["pts"] = points_json(points);
    j["width"] = width.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::text(Num x, Num y, Num w, Num h, std::string text, Num size, std::string color, std::string align,
                   std::string font) {
    Json& j = add("text");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["w"] = w.to_json(), j["h"] = h.to_json();
    j["text"] = std::move(text);
    j["size"] = size.to_json();
    j["color"] = std::move(color);
    j["align"] = std::move(align);
    j["font"] = std::move(font);
    return *this;
}

Scene& Scene::icon(Num x, Num y, Num size, std::string name, std::string color) {
    Json& j = add("icon");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["size"] = size.to_json();
    j["name"] = std::move(name);
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::grad(Num x, Num y, Num w, Num h, Num radius, std::string from, std::string to, bool vertical) {
    Json& j = add("grad");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["w"] = w.to_json(), j["h"] = h.to_json();
    j["r"] = radius.to_json();
    j["from"] = std::move(from);
    j["to"] = std::move(to);
    j["dir"] = vertical ? "v" : "h";
    return *this;
}

Scene& Scene::glow(Num cx, Num cy, Num r, std::string color) {
    Json& j = add("glow");
    j["cx"] = cx.to_json(), j["cy"] = cy.to_json(), j["r"] = r.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::rglow(Num x, Num y, Num w, Num h, Num radius, Num spread, std::string color) {
    Json& j = add("rglow");
    j["x"] = x.to_json(), j["y"] = y.to_json(), j["w"] = w.to_json(), j["h"] = h.to_json();
    j["r"] = radius.to_json();
    j["spread"] = spread.to_json();
    j["color"] = std::move(color);
    return *this;
}

Scene& Scene::alpha(Num a) {
    if (ops_.size() > 0) ops_.at(ops_.size() - 1)["alpha"] = a.to_json();
    return *this;
}

// ---------------------------------------------------------------- ThemeDef

ThemeDef& ThemeDef::name(const std::string& lang, const std::string& text) {
    names_[lang] = text;
    return *this;
}

ThemeDef& ThemeDef::dark(bool on) {
    dark_ = on;
    return *this;
}

ThemeDef& ThemeDef::color(const std::string& token, const std::string& value) {
    colors_[token] = value;
    return *this;
}

ThemeDef& ThemeDef::radius(float dp) {
    radius_ = dp;
    return *this;
}

ThemeDef& ThemeDef::animated(bool on) {
    animated_ = on;
    return *this;
}

Scene& ThemeDef::screen(const std::string& screen, Layer layer) {
    return scenes_["screen/" + screen + (layer == Layer::Under ? "/under" : "/over")];
}

Scene& ThemeDef::tile(Layer layer) { return scenes_[layer == Layer::Under ? "tile/under" : "tile/over"]; }

Scene& ThemeDef::card(Layer layer) { return scenes_[layer == Layer::Under ? "card/under" : "card/over"]; }

Json ThemeDef::to_json() const {
    Json j = Json::object();
    j["format"] = 1;
    j["id"] = id_;
    if (!names_.empty()) {
        Json n = Json::object();
        for (const auto& [lang, text] : names_) n[lang] = text;
        j["name"] = std::move(n);
    }
    j["base"] = dark_ ? "dark" : "light";
    if (radius_ > 0) j["radius"] = radius_;
    if (animated_) j["animated"] = true;
    if (!colors_.empty()) {
        Json p = Json::object();
        for (const auto& [token, value] : colors_) p[token] = value;
        j["palette"] = std::move(p);
    }
    Json screens = Json::object();
    for (const auto& [key, scene] : scenes_) {
        if (scene.empty()) continue;
        // "screen/<id>/<layer>", "tile/<layer>", "card/<layer>"
        size_t last = key.rfind('/');
        std::string layer = key.substr(last + 1), where = key.substr(0, last);
        if (where.compare(0, 7, "screen/") == 0) {
            Json& s = screens[where.substr(7)];
            s[layer] = scene.ops();
        } else {
            j[where][layer] = scene.ops();
        }
    }
    if (screens.size() > 0) j["screens"] = std::move(screens);
    return j;
}

bool ThemeDef::save(const std::string& path, std::string* error) const {
    if (!save_json_file(path, to_json())) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace facet::sdk
