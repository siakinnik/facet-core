#include "plugins/permissions.h"

namespace facet::plugins {

const std::vector<PermissionInfo>& all_permissions() {
    static const std::vector<PermissionInfo> list = {
        {"network", Level::Dangerous, false, "Network", "Internet and local network access."},
        {"camera", Level::Dangerous, true, "Camera",
         "Use the cameras. While a module uses a camera, other programs cannot."},
        {"microphone", Level::Dangerous, true, "Microphone", "Record sound from the microphones."},
        {"audio", Level::Normal, false, "Sound output", "Play sound through the speakers."},
        {"gpu", Level::Normal, false, "Graphics acceleration", "Draw with the graphics processor."},
        {"storage.downloads", Level::Dangerous, false, "Downloads folder",
         "Read and save files in the shared Downloads folder."},
        {"system.stats", Level::Dangerous, false, "System information",
         "Read-only view of the whole system: load, all processes, sensors and disks, like any user of this "
         "device."},
        {"notifications", Level::Dangerous, false, "Notifications", "Show notifications and incoming calls."},
        {"background", Level::Normal, false, "Run in the background",
         "Keep working while none of its screens is open. Without it the module is paused in the background."},
        {"wake_lock", Level::Normal, false, "Keep the screen on", "Keep the screen on while it needs it."},
        {"display.power", Level::Special, false, "Screen power", "Turn the screen on and off."},
        {"notifications.distributor", Level::Special, false, "Notification distributor",
         "Post notifications on behalf of other modules and apps, and see every notification of the system."},
        {"wayland.compositor", Level::Special, false, "Display server",
         "Run the Wayland display server: full access to the screen, graphics and input devices."},
        {"wayland.window", Level::Normal, false, "Windows", "Show windows of desktop apps."},
        {"wayland.clipboard", Level::Dangerous, false, "Clipboard", "Read and change the clipboard."},
        {"wayland.screencopy", Level::Dangerous, true, "Screen capture",
         "Capture the screen, e.g. for screen sharing in a call."},
    };
    return list;
}

const PermissionInfo* permission_info(const std::string& name) {
    for (const auto& p : all_permissions())
        if (name == p.name) return &p;
    return nullptr;
}

const char* grant_name(Grant g) {
    switch (g) {
        case Grant::Allow: return "allow";
        case Grant::Ask: return "ask";
        case Grant::Deny: return "deny";
        case Grant::Unset: break;
    }
    return "";
}

Grant grant_from(const std::string& s) {
    if (s == "allow") return Grant::Allow;
    if (s == "ask") return Grant::Ask;
    if (s == "deny") return Grant::Deny;
    return Grant::Unset;
}

}  // namespace facet::plugins
