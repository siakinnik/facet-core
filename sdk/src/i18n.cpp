#include "facet/i18n.h"

namespace facet::i18n {

std::string format(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(pattern.size() + 16);
    size_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            if (next < args.size()) out += args[next];
            ++next;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

std::string normalize(std::string_view locale) {
    std::string lang;
    for (char c : locale) {
        if (c == '_' || c == '.' || c == '-' || c == '@') break;
        lang += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (lang.empty() || lang == "c" || lang == "posix") return "en";
    return lang;
}

bool Catalog::set_language(const std::string& lang) {
    auto it = tables_.find(lang);
    if (it == tables_.end()) {
        lang_ = "en";
        current_ = nullptr;
        return lang == "en";
    }
    lang_ = lang;
    current_ = it->second;
    return true;
}

std::string Catalog::tr(std::string_view key) const {
    if (current_) {
        auto it = current_->find(key);
        if (it != current_->end()) return std::string(it->second);
    }
    return std::string(key);
}

}  // namespace facet::i18n
