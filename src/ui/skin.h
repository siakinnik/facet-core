// A theme pack's look (sdk/include/facet/theme.h): the palette it changes, the
// corner radius and the drawings it puts under and over screens, tiles and
// cards. Read from the pack's JSON file, checked, and kept for drawing.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "facet/json.h"
#include "gfx/types.h"
#include "ui/theme.h"

namespace facet::ui {

class Skin {
public:
    // The drawings of one place: either may be empty.
    struct Hook {
        Json under = Json::array(), over = Json::array();
        bool has_under = false, has_over = false;
    };

    // Reads and checks a theme file; null and `error` set when it cannot be used.
    static std::shared_ptr<Skin> load(const std::string& path, std::string& error);
    static std::shared_ptr<Skin> parse(const Json& j, std::string& error);

    bool dark = true;
    bool animated = false;  // the pack says it has moving parts
    float radius = 0;       // dp, 0: the default

    // The base palette with the pack's colours over it.
    void apply(Palette& p) const;
    // The pack's own colour names, for its drawings.
    std::unordered_map<std::string, gfx::Color> custom;

    const Hook* screen(const std::string& id) const;  // exact id, or null
    Hook everywhere;                                   // "*"
    Hook tile, card;

    static constexpr size_t kMaxFileBytes = 256 * 1024;
    static constexpr size_t kMaxOpsPerHook = 600;
    static constexpr size_t kMaxOpsTotal = 6000;

private:
    std::unordered_map<std::string, gfx::Color> core_;  // token name -> colour, for apply()
    std::unordered_map<std::string, Hook> screens_;
};

}  // namespace facet::ui
