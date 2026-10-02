// Plugin containers: every plugin process runs in its own Linux namespaces
// (mount, PID, IPC, UTS and, without the "network" permission, network) as
// its own unprivileged user, with a minimal root file system that only
// contains what its granted permissions allow. See docs/ARCHITECTURE.md §3.4.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace facet::plugins::sandbox {

// Permissions with an effect on the container or the core's protocol.
constexpr const char* kCamera = "camera";
constexpr const char* kMicrophone = "microphone";
constexpr const char* kAudio = "audio";
constexpr const char* kGpu = "gpu";
constexpr const char* kDownloads = "storage.downloads";
constexpr const char* kNetwork = "network";
constexpr const char* kSystemStats = "system.stats";
constexpr const char* kDisplayPower = "display.power";
constexpr const char* kNotifications = "notifications";
constexpr const char* kDistributor = "notifications.distributor";
constexpr const char* kBackground = "background";
constexpr const char* kWakeLock = "wake_lock";
constexpr const char* kCompositor = "wayland.compositor";
constexpr const char* kSurface = "display.surface";

// Host device nodes a permission gives access to (camera: /dev/video*, ...).
std::vector<std::string> device_nodes(const std::string& permission);

struct Spec {
    std::string id;
    std::string plugin_dir;  // host path, mounted read-only at /plugin
    std::string exec;        // file name inside plugin_dir
    std::string data_dir;    // host path, mounted read-write at /data
    uid_t uid = 0;           // the plugin's own user (and group)
    std::vector<std::string> granted;
    std::vector<std::string> env;  // "KEY=value", the complete environment
    std::string surface_dir;       // host path, mounted read-write at /run/facet/surface
};

// Host directory (on tmpfs) where a plugin's Surface buffers live; emptied
// and handed to `uid` (0 = leave the owner) by prepare_surface_dir().
std::string surface_dir(const std::string& id);
bool prepare_surface_dir(const std::string& id, uid_t uid);
constexpr const char* kSurfaceDirInContainer = "/run/facet/surface";

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

// Transient permissions: creates (or removes) the permission's device nodes
// in the running container of `pid`, owned by the plugin's user. Returns an
// English error, empty on success.
std::string attach_devices(pid_t pid, uid_t uid, const std::string& permission);
std::string detach_devices(pid_t pid, const std::string& permission);

// Sends `sig` to every process of the plugin: its container (PID namespace
// of `pid`) or, without containers, its process group.
void signal_all(pid_t pid, bool sandboxed, int sig);

}  // namespace facet::plugins::sandbox
