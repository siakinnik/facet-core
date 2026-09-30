// Example plugin: a PIN pad drawn with the canvas widget.
//
// Shows the parts a real plugin needs: the message loop (facet::sdk::Plugin),
// a screen built from standard widgets plus custom drawing (Screen::canvas),
// touch targets, translations and state that never leaves the process.
// The PIN is only kept in memory and is never logged or written anywhere.
#include <algorithm>
#include <string>

#include "facet/plugin.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::Canvas;
using facet::sdk::Plugin;
using facet::sdk::Screen;

namespace {

constexpr size_t kMaxDigits = 8;
constexpr size_t kMinDigits = 4;

class PinPad {
public:
    explicit PinPad(Plugin& plugin) : plugin_(plugin) {}

    void on_event(const std::string& id, const Json& value) {
        if (id == "pad") {
            const std::string key = value.str();
            if (key == "back") {
                if (!pin_.empty()) pin_.pop_back();
            } else if (key == "clear") {
                pin_.clear();
            } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9' && pin_.size() < kMaxDigits) {
                pin_ += key;
            }
            accepted_ = false;
        } else if (id == "name") {
            name_ = value.str();
        } else if (id == "amount") {
            amount_ = value.str();
        } else if (id == "secret") {
            secret_ = value.str();  // secure field: arrives here only, never via keyboard plugins
        } else if (id == "ok" && pin_.size() >= kMinDigits) {
            accepted_ = true;
            pin_.clear();  // a real plugin would use it here, then forget it
        }
        refresh();
    }

    void refresh() {
        plugin_.set_tile(accepted_ ? plugin_.tr("PIN accepted") : plugin_.tr("Enter a PIN"));
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    // Row of dots, one per possible digit; entered ones are filled.
    Canvas dots(float width) const {
        Canvas c;
        const float r = 9, gap = 30;
        float x0 = width / 2 - gap * float(kMaxDigits - 1) / 2;
        for (size_t i = 0; i < kMaxDigits; ++i) {
            float cx = x0 + gap * float(i);
            if (i < pin_.size()) c.circle(cx, 28, r, "accent");
            else c.ring(cx, 28, r, 2, "track");
        }
        return c;
    }

    // 3x4 keypad centred in the content column.
    Canvas keypad(float width, float& height) const {
        Canvas c;
        const float gap = 14;
        float key = std::min(104.f, (width - 2 * gap) / 3);
        float x0 = (width - (3 * key + 2 * gap)) / 2;
        const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "clear", "0", "back"};
        for (int i = 0; i < 12; ++i) {
            float x = x0 + float(i % 3) * (key + gap), y = float(i / 3) * (key * 0.75f + gap);
            float h = key * 0.75f;
            std::string k = keys[i];
            c.rrect(x, y, key, h, 18, "surface", k, "surface_pressed");
            if (k == "back") c.icon(x + key / 2 - 14, y + h / 2 - 14, 28, "back", "text");
            else if (k == "clear") c.text(x, y, key, h, plugin_.tr("Clear"), 18, "text_dim", "center");
            else c.text(x, y, key, h, k, 32, "text", "center", "light");
        }
        height = 4 * (key * 0.75f + gap) - gap;
        return c;
    }

    Screen build() const {
        const float width = float(plugin_.content_width());
        Screen s(plugin_.tr("PIN pad demo"));
        s.section(plugin_.tr("Custom drawing"));
        s.canvas("dots", 56, dots(width));
        float pad_h = 0;
        Canvas pad = keypad(width, pad_h);
        s.canvas("pad", pad_h, pad);
        if (pin_.size() >= kMinDigits) s.button("ok", plugin_.tr("Confirm"), "primary");
        if (accepted_) s.info(plugin_.tr("Result"), plugin_.tr("PIN accepted"), "good");
        s.note(plugin_.tr("Drawn by the plugin with canvas operations; colours follow the theme. "
                          "The digits never leave this plugin and are not stored."));

        s.section(plugin_.tr("Text input"));
        s.text_field("name", plugin_.tr("Name"), name_, plugin_.tr("Tap to type"));
        s.text_field("amount", plugin_.tr("Amount"), amount_, "0", false, "number", 12);
        s.text_field("secret", plugin_.tr("Password"), secret_, plugin_.tr("Hidden"), true);
        if (!submitted_.empty()) s.info(plugin_.tr("Submitted"), submitted_);
        s.note(plugin_.tr("The password field is secure: it always uses the built-in keyboard, "
                          "so keyboard plugins never see it."));
        return s;
    }

  public:
    void on_submit(const std::string& id, const std::string& text) {
        submitted_ = id == "secret" ? plugin_.tr("password ({} characters)", {std::to_string(text.size())}) : text;
        refresh();
    }

  private:
    Plugin& plugin_;
    std::string pin_;
    std::string name_, amount_, secret_, submitted_;
    bool accepted_ = false;
};

}  // namespace

int main() {
    Plugin plugin("example-pinpad", "0.0.1");
    example::register_translations(plugin.catalog());
    PinPad app(plugin);
    plugin.on_hello = [&](const Json&) { app.refresh(); };
    plugin.on_event = [&](const std::string& id, const Json& v) { app.on_event(id, v); };
    plugin.on_submit = [&](const std::string& id, const std::string& text) { app.on_submit(id, text); };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_layout = [&](int) { app.refresh(); };
    return plugin.run();
}
