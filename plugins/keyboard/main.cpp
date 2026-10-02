// Default on-screen keyboard, installed with the core. Any plugin with the
// input.keyboard (manifest "provides") can replace it (Settings -> Input -> Keyboard).
//
// Protocol: the core sends keyboard_show (field mode, width, languages) and
// keyboard_key (tapped hit id); the plugin answers with keyboard_ui (canvas)
// and input actions. It never sees the contents of any field.
#include <string>
#include <vector>

#include "facet/keyboard.h"
#include "facet/plugin.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::Keyboard;
using facet::sdk::KeyAction;
using facet::sdk::Plugin;

int main() {
    Plugin plugin("keyboard", "0.3.0");  // keep in sync with manifest.json
    keyboard_plugin::register_translations(plugin.catalog());
    Keyboard kb;
    float width = 400;
    bool open = false;

    auto redraw = [&] {
        kb.set_labels({plugin.tr("Done"), plugin.tr("space"), "?123", "ABC"});
        float height = 0;
        facet::sdk::Canvas canvas = kb.build(width, height);
        plugin.keyboard_ui(height, canvas);
    };

    plugin.on_keyboard_show = [&](const std::string& mode, float w, const std::vector<std::string>& langs) {
        Plugin::log("shown for a %s field", mode.c_str());
        kb.configure(Keyboard::mode_from(mode), langs);
        width = w;
        open = true;
        redraw();
    };
    plugin.on_keyboard_key = [&](const std::string& hit) {
        KeyAction a = kb.press(hit);
        switch (a.kind) {
            case KeyAction::Kind::Insert: plugin.input("insert", a.text); break;
            case KeyAction::Kind::Backspace: plugin.input("backspace"); break;
            case KeyAction::Kind::Enter: plugin.input("enter"); break;
            case KeyAction::Kind::Hide: plugin.input("hide"); break;
            case KeyAction::Kind::None: break;
        }
        redraw();  // shift / symbols / language may have changed
    };
    plugin.on_keyboard_hide = [&] { open = false; };
    plugin.on_locale = [&](const std::string&) {
        if (open) redraw();
    };
    return plugin.run();
}
