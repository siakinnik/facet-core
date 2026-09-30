// Russian translation of the core UI. Keys are the English source strings.
#include "i18n/i18n.h"

namespace facet {

const std::vector<Language>& languages() {
    static const std::vector<Language> list = {
        {"en", "English"},
        {"ru", "Русский"},
    };
    return list;
}

namespace i18n_tables {

const i18n::Table& ru() {
    static const i18n::Table table = {
        // Splash
        {"Starting plugins", "Запуск плагинов"},
        {"Waiting for the system", "Ожидание системы"},
        {"Ready", "Готово"},

        // Date: "{weekday}, {day} {month}"
        {"{}, {} {}", "{}, {} {}"},
        {"Sunday", "воскресенье"},
        {"Monday", "понедельник"},
        {"Tuesday", "вторник"},
        {"Wednesday", "среда"},
        {"Thursday", "четверг"},
        {"Friday", "пятница"},
        {"Saturday", "суббота"},
        // Months in the genitive case, as used in dates.
        {"January", "января"},
        {"February", "февраля"},
        {"March", "марта"},
        {"April", "апреля"},
        {"May", "мая"},
        {"June", "июня"},
        {"July", "июля"},
        {"August", "августа"},
        {"September", "сентября"},
        {"October", "октября"},
        {"November", "ноября"},
        {"December", "декабря"},

        // Menu
        {"Dashboard", "Дашборд"},
        {"Clock", "Часы"},
        {"Error", "Ошибка"},
        {"Crashed, restarting…", "Сбой, перезапуск…"},

        // Plugin states
        {"disabled", "выключен"},
        {"starting…", "запуск…"},
        {"running", "работает"},
        {"crashed, restarting", "сбой, перезапуск"},
        {"failed", "ошибка"},
        {"stopping…", "остановка…"},

        // Settings
        {"Settings", "Настройки"},
        {"Appearance", "Оформление"},
        {"Theme", "Тема"},
        {"Dark", "Тёмная"},
        {"Light", "Светлая"},
        {"Auto (day/night)", "Авто (день/ночь)"},
        {"Brightness", "Яркость"},
        {"no backlight control", "нет управления подсветкой"},
        {"Language", "Язык"},
        {"Plugins", "Плагины"},
        {"No plugins found", "Плагины не найдены"},
        {"Restart “{}”", "Перезапустить «{}»"},
        {"System", "Система"},
        {"Network (IP)", "Сеть (IP)"},
        {"no network", "нет сети"},
        {"Display", "Экран"},
        {"Version", "Версия"},
        {"Data: {}", "Данные: {}"},

        // Date & time
        {"Date & time", "Дата и время"},
        {"Time zone", "Часовой пояс"},
        {"City", "Город"},
        {"System ({})", "Системный ({})"},
        {"unknown", "неизвестно"},
        {"Local time", "Местное время"},
        // tzdata regions
        {"Africa", "Африка"},
        {"America", "Америка"},
        {"Antarctica", "Антарктида"},
        {"Arctic", "Арктика"},
        {"Asia", "Азия"},
        {"Atlantic", "Атлантика"},
        {"Australia", "Австралия"},
        {"Europe", "Европа"},
        {"Indian", "Индийский океан"},
        {"Pacific", "Тихий океан"},

        // Network and build info
        {"Network", "Сеть"},
        {"Connection", "Подключение"},
        {"Wi-Fi", "Wi-Fi"},
        {"Ethernet", "Ethernet"},
        {"Offline", "Нет сети"},
        {"Signal", "Сигнал"},
        {"IP address", "IP-адрес"},
        {"Build", "Сборка"},
        {"Commit", "Коммит"},
        {"Repository", "Репозиторий"},
        {"(modified)", "(с изменениями)"},
        {"dev", "dev (разработка)"},
        {"release", "релиз"},

        // Input
        {"Input", "Ввод"},
        {"Keyboard", "Клавиатура"},
        {"Built-in", "Встроенная"},
        {"Done", "Готово"},
        {"space", "пробел"},

        // Plugin screen
        {"Status", "Состояние"},
        {"Plugin", "Плагин"},
        {"Restart", "Перезапустить"},

        // Plugin failures
        {"executable not found: {}", "не найден исполняемый файл: {}"},
        {"pipe failed: {}", "ошибка pipe: {}"},
        {"fork failed: {}", "ошибка fork: {}"},
        {"process exited", "процесс завершился"},
        {"process closed its output", "процесс закрыл канал"},
        {"protocol violation (invalid messages)", "нарушение протокола (невалидные сообщения)"},
        {"protocol violation (message too long)", "нарушение протокола (слишком длинное сообщение)"},
        {"invalid hello", "неверный hello"},
        {"no hello within 5 s", "не ответил на hello за 5 с"},
        {"hung (no reply to ping)", "завис (нет ответа на ping)"},
        {"not reading messages", "не читает сообщения"},
        {"exit code {}", "код выхода {}"},
        {"signal {}", "сигнал {}"},
        {"Too many crashes in a row — plugin stopped.", "Слишком много сбоев подряд — плагин остановлен."},
    };
    return table;
}

}  // namespace i18n_tables
}  // namespace facet
