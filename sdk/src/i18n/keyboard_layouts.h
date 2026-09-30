// Keyboard layouts (language data). Letters live here, in the i18n directory,
// like every other non-English text.
#pragma once

#include <string>

namespace facet::sdk::layouts {

struct Layout {
    const char* lang;
    const char* label;    // shown on the language key
    const char* rows[3];  // UTF-8, one character per key
};

// Layout for a language code; English if unknown.
const Layout& for_lang(const std::string& lang);
// Digits and punctuation layer.
const char* const* symbol_rows();

}  // namespace facet::sdk::layouts
