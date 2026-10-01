// Plugin containers: every plugin process runs in its own Linux namespaces
// (mount, PID, IPC, UTS and, without the "network" permission, network) as
// its own unprivileged user, with a minimal root file system that only
// contains what its granted permissions allow. See docs/ARCHITECTURE.md §3.4.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace facet::plugins::sandbox {

// Permissions the sandbox knows how to grant.
constexpr const char* kCamera = "camera";
constexpr const char* kNetwork = "network";
constexpr const char* kSystemStats = "system.stats";
constexpr const char* kDisplayPower = "display.power";

struct Spec {
    std::string id;
    std::string plugin_dir;  // host path, mounted read-only at /plugin
    std::string exec;        // file name inside plugin_dir
    std::string data_dir;    // host path, mounted read-write at /data
    uid_t uid = 0;           // the plugin's own user (and group)
    std::vector<std::string> granted;
    std::vector<std::string> env;  // "KEY=value", the complete environment
};

// True when containers can be used: Facet runs as root and FACET_SANDBOX is
// not "0". Otherwise plugins run as plain processes (development).
bool available();

// Prepares host-side state (data dir ownership, staging dir). Returns an
// English error message, empty on success.
std::string prepare(const Spec& spec);

// Starts the plugin in a new container with the given stdio pipe ends.
// Returns the pid (in Facet's namespace) or -1 with errno set.
pid_t spawn(const Spec& spec, int stdin_fd, int stdout_fd, int stderr_fd);

// Exit code of a child that could not set up its container.
constexpr int kSetupFailed = 125;

}  // namespace facet::plugins::sandbox
