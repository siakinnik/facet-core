// Russian translation of the example plugin.
#include "i18n/i18n.h"

namespace example {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"PIN pad demo", "Демо PIN-панели"},
        {"Custom drawing", "Своя отрисовка"},
        {"Clear", "Сброс"},
        {"Confirm", "Подтвердить"},
        {"Result", "Результат"},
        {"PIN accepted", "PIN принят"},
        {"Enter a PIN", "Введите PIN"},
        {"Drawn by the plugin with canvas operations; colours follow the theme. "
         "The digits never leave this plugin and are not stored.",
         "Нарисовано самим плагином командами холста; цвета следуют теме. "
         "Цифры не покидают плагин и нигде не сохраняются."},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace example
