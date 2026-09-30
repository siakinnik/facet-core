// Core localisation. User-visible strings are written in English in the code
// and wrapped in tr(); translations live next to this file (one .cpp per
// language). To add a language: copy ru.cpp, translate, register it in
// i18n.cpp and add it to languages().
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "facet/i18n.h"

namespace facet {

i18n::Catalog& catalog();

// The const char* overloads make string literals an exact match (otherwise
// they convert equally well to std::string_view and i18n::Text).
inline std::string tr(const char* key) { return catalog().tr(key); }
inline std::string tr(const char* key, const std::vector<std::string>& args) { return catalog().tr(key, args); }
inline std::string tr(const std::string& key) { return catalog().tr(key); }
inline std::string tr(std::string_view key) { return catalog().tr(key); }
inline std::string tr(std::string_view key, const std::vector<std::string>& args) { return catalog().tr(key, args); }
inline std::string tr(const i18n::Text& t) { return catalog().render(t); }

struct Language {
    const char* code;         // "en", "ru", ...
    const char* native_name;  // shown in the language picker
};
const std::vector<Language>& languages();

// Language from FACET_LANG / LC_ALL / LC_MESSAGES / LANG, if supported; else "en".
std::string detect_language();

}  // namespace facet
