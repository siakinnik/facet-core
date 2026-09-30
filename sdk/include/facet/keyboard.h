// On-screen keyboard layout and state, shared by the core's built-in fallback
// keyboard and keyboard plugins. It draws itself as canvas operations and turns
// taps (hit ids) into actions for the focused text field. A keyboard plugin
// only has to forward keyboard_show/keyboard_key to it (see plugins/keyboard).
#pragma once

#include <string>
#include <vector>

#include "facet/plugin.h"

namespace facet::sdk {

struct KeyAction {
    enum class Kind { None, Insert, Backspace, Enter, Hide };
    Kind kind = Kind::None;
    std::string text;  // for Insert
};

class Keyboard {
public:
    enum class Mode { Text, Number };
    struct Labels {
        std::string done = "Done", space = "space", symbols = "?123", letters = "ABC";
    };

    // Starts a new input session: mode of the field and layout languages
    // ("en", "ru"); the first language is shown first.
    void configure(Mode mode, std::vector<std::string> langs);
    void set_labels(Labels labels) { labels_ = std::move(labels); }

    // Canvas for a keyboard `width` dp wide; `height` receives its height in dp.
    Canvas build(float width, float& height) const;
    // Applies a tapped key. Layout changes (shift, symbols, language) return
    // Kind::None; rebuild the canvas afterwards.
    KeyAction press(const std::string& hit);

    static Mode mode_from(const std::string& name) { return name == "number" ? Mode::Number : Mode::Text; }

private:
    std::vector<std::vector<std::string>> letter_rows() const;
    Canvas build_text(float width, float& height) const;
    Canvas build_number(float width, float& height) const;

    Mode mode_ = Mode::Text;
    std::vector<std::string> langs_{"en"};
    size_t lang_ = 0;
    bool symbols_ = false;
    bool shift_ = false;
    Labels labels_;
};

}  // namespace facet::sdk
