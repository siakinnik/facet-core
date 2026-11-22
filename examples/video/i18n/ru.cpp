// Russian translation of the video example. Keys are the English source strings.
#include "i18n/i18n.h"

namespace example_video {

namespace {

const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Video demo", "Демо видео"},
        {"stopped", "остановлено"},
        {"loading…", "загрузка…"},
        {"paused", "пауза"},
        {"playing", "играет"},
        {"Volume {}%", "Громкость {}%"},
    };
    return table;
}

}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace example_video
