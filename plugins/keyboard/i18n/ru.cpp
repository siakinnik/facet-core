// Russian labels of the keyboard plugin.
#include "i18n/i18n.h"

namespace keyboard_plugin {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Done", "Готово"},
        {"space", "пробел"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace keyboard_plugin
