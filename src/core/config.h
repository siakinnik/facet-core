// Persistent core settings (JSON file) and filesystem locations.
#pragma once

#include <string>
#include <vector>

#include "facet/json.h"

namespace facet {

namespace paths {
// $FACET_DATA, else /var/lib/facet when writable, else ~/.local/share/facet.
const std::string& data_root();
// $FACET_PLUGIN_PATH (':'-separated), <exe dir>/plugins, <prefix>/lib/facet/plugins
// relative to the executable, system and store dirs.
std::vector<std::string> plugin_dirs();
bool mkdirs(const std::string& path);
// Directory of the running executable.
std::string exe_dir();
}  // namespace paths

class Config {
public:
    void load(const std::string& path);
    // Writes at most once per second of changes; call regularly and at exit.
    void save_if_dirty(double now, bool force = false);

    const Json& get(const std::string& key) const { return root_[key]; }
    int get_int(const std::string& key, int def) const { return root_[key].as_int(def); }
    bool get_bool(const std::string& key, bool def) const { return root_[key].as_bool(def); }
    std::string get_str(const std::string& key, const std::string& def) const { return root_[key].as_string(def); }
    void set(const std::string& key, Json value);

private:
    std::string path_;
    Json root_ = Json::object();
    bool dirty_ = false;
    double dirty_since_ = 0;
};

}  // namespace facet
