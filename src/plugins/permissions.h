// The permissions plugins can request (docs/ARCHITECTURE.md §3.4).
//
// Protection levels, as on Android:
// - normal:    granted without asking (listed, the user can revoke them);
// - dangerous: the user decides before the plugin first starts;
// - special:   like dangerous, shown with a warning (system-wide powers).
// Device permissions can be "transient": the plugin asks at run time and holds
// access only while it uses the device (e.g. during a call).
#pragma once

#include <string>
#include <vector>

namespace facet::plugins {

enum class Level { Normal, Dangerous, Special };

struct PermissionInfo {
    const char* name;
    Level level;
    bool can_be_transient;
    const char* title;        // English UI string (translated with tr())
    const char* description;  // English UI string
};

// nullptr for names this core does not know (listed as unknown, never granted).
const PermissionInfo* permission_info(const std::string& name);
const std::vector<PermissionInfo>& all_permissions();

// What the user chose for one permission of one plugin.
enum class Grant {
    Unset,  // not reviewed yet
    Allow,  // granted; transient ones: given on request without asking
    Ask,    // transient only: ask every time the plugin requests it
    Deny,
};
const char* grant_name(Grant g);
Grant grant_from(const std::string& s);

}  // namespace facet::plugins
