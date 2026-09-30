// Localisation shared by the core and plugins.
//
// Source strings are English and double as lookup keys (gettext style):
//   catalog.tr("Settings")            -> "Settings" or its translation
//   catalog.tr("Restart “{}”", {name}) -> positional "{}" arguments
// A language is a Table mapping English -> translation; missing entries fall
// back to English. Translation tables live in each project's i18n/ directory.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace facet::i18n {

using Table = std::unordered_map<std::string_view, std::string_view>;

// A message kept untranslated (key + arguments) so it can be rendered later
// in whatever language is active at display time.
struct Text {
    std::string key;
    std::vector<std::string> args;

    Text() = default;
    Text(std::string k, std::vector<std::string> a = {}) : key(std::move(k)), args(std::move(a)) {}
    Text(const char* k) : key(k) {}
    bool empty() const { return key.empty(); }
    bool operator==(const Text& o) const { return key == o.key && args == o.args; }
};

// Replaces each "{}" in `pattern` with the next argument.
std::string format(std::string_view pattern, const std::vector<std::string>& args);
// "ru_RU.UTF-8" -> "ru"; empty/"C"/"POSIX" -> "en".
std::string normalize(std::string_view locale);

class Catalog {
public:
    void add(const std::string& lang, const Table& table) { tables_[lang] = &table; }
    bool has(const std::string& lang) const { return lang == "en" || tables_.count(lang) != 0; }
    // Switches language; unknown languages fall back to English. Returns
    // true if the requested language is available.
    bool set_language(const std::string& lang);
    const std::string& language() const { return lang_; }

    std::string tr(std::string_view key) const;
    std::string tr(std::string_view key, const std::vector<std::string>& args) const { return format(tr(key), args); }
    std::string render(const Text& t) const { return t.empty() ? std::string() : tr(t.key, t.args); }

private:
    std::map<std::string, const Table*> tables_;
    std::string lang_ = "en";
    const Table* current_ = nullptr;
};

}  // namespace facet::i18n
