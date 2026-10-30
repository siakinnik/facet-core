// Keyboard layouts. To add a language, add a Layout with three rows of keys.
#include "i18n/keyboard_layouts.h"

namespace facet::sdk::layouts {

namespace {
const Layout kLayouts[] = {
    {"en", "EN", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}},
    {"ru", "RU", {"йцукенгшщзхъ", "фывапролджэ", "ячсмитьбюё"}},
};
const char* const kSymbols[3] = {"1234567890", "-/:;()&@\"'", ".,?!#%*+="};
// The rest of what passwords and addresses use; with the first layer every ASCII sign is there.
const char* const kSymbols2[3] = {"[]{}^~<>|\\", "_$€£¥°§`№", ".,?!'\"-"};
}  // namespace

const Layout& for_lang(const std::string& lang) {
    for (const auto& l : kLayouts)
        if (lang == l.lang) return l;
    return kLayouts[0];
}

const char* const* symbol_rows(int page) { return page == 1 ? kSymbols2 : kSymbols; }

}  // namespace facet::sdk::layouts
