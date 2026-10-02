// Russian translation of the showcase example.
#include "i18n/i18n.h"

namespace showcase {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"SDK showcase", "Демо SDK"},
        {"Hello from the showcase", "Привет из демо"},
        {"Notification number {}", "Уведомление номер {}"},
        {"Reply", "Ответить"},
        {"Video call", "Видеозвонок"},
        {"To show that transient access works.", "Чтобы показать, как работает временный доступ."},
        {"asking for the camera…", "запрашиваю камеру…"},
        {"camera released", "камера освобождена"},
        {"Pretending to sync", "Делаю вид, что синхронизирую"},
        {"granted", "выдано"},
        {"not granted", "не выдано"},
        {"notification {}: {}", "уведомление {}: {}"},
        {"Sent: {}", "Отправлено: {}"},
        {"Nothing sent yet", "Пока ничего не отправлено"},
        {"Notifications", "Уведомления"},
        {"The notifications permission is off: nothing will appear. The plugin keeps working.",
         "Разрешение на уведомления выключено: ничего не появится. Плагин продолжает работать."},
        {"Send a notification", "Отправить уведомление"},
        {"Simulate an incoming call", "Изобразить входящий звонок"},
        {"Clear the badge", "Убрать счётчик"},
        {"Call", "Звонок"},
        {"accepted", "принят"},
        {"Camera while in use", "Камера на время использования"},
        {"Camera devices I can see", "Камер видно внутри"},
        {"Use the camera", "Взять камеру"},
        {"Release the camera", "Отпустить камеру"},
        {"System", "Система"},
        {"Keep the screen on", "Не выключать экран"},
        {"Background work", "Работа в фоне"},
        {"Permissions now", "Разрешения сейчас"},
        {"Last event", "Последнее событие"},
        {"Own picture", "Своё изображение"},
        {"The “Own picture” permission is off, so there is nothing to draw on.",
         "Разрешение «Своё изображение» выключено, рисовать не на чем."},
        {"Touches", "Касаний"},
        {"Full screen (swipe down from the top to leave)", "На весь экран (выход — смахнуть вниз от верхнего края)"},
        {"Type with Facet's keyboard", "Печатать клавиатурой Facet"},
        {"Typed", "Набрано"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace showcase
