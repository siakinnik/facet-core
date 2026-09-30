#include "i18n/i18n.h"

#include <cstdlib>

namespace facet {

namespace i18n_tables {
const i18n::Table& ru();
}

i18n::Catalog& catalog() {
    static i18n::Catalog c = [] {
        i18n::Catalog cat;
        cat.add("ru", i18n_tables::ru());
        return cat;
    }();
    return c;
}

std::string detect_language() {
    for (const char* var : {"FACET_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        std::string lang = i18n::normalize(v);
        return catalog().has(lang) ? lang : "en";
    }
    return "en";
}

}  // namespace facet
