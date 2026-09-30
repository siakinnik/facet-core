// Keyboard layouts. To add a language, add a Layout with three rows of keys.
#include "i18n/keyboard_layouts.h"

namespace facet::sdk::layouts {

namespace {
const Layout kLayouts[] = {
    {"en", "EN", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}},
    {"ru", "RU", {"йцукенгшщзхъ", "фывапролджэ", "ячсмитьбюё"}},
};
const char* const kSymbols[3] = {"1234567890", "-/:;()&@\"'", ".,?!#%*+="};
}  // namespace

const Layout& for_lang(const std::string& lang) {
    for (const auto& l : kLayouts)
        if (lang == l.lang) return l;
    return kLayouts[0];
}

const char* const* symbol_rows() { return kSymbols; }

}  // namespace facet::sdk::layouts
