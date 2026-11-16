// Plugin containers: every plugin process runs in its own Linux namespaces
// (mount, PID, IPC, UTS and, without the "network" permission, network) as
// its own unprivileged user, with a minimal root file system that only
// contains what its granted permissions allow. See docs/ARCHITECTURE.md §3.4.
// On kernels without some of these namespaces the container uses those that
// exist; without a mount namespace plugins run as plain processes, each as
// its own user (see Mode).
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
    struct Bind {
        std::string host, inside;  // `inside` is an absolute path in the container
        bool read_only = false;
    };
    std::vector<Bind> binds;  // capability endpoints (see endpoint_dir()), the OpenGL package
};

// Host directory (on tmpfs) where a plugin's Surface buffers live; emptied
// and handed to `uid` (0 = leave the owner) by prepare_surface_dir().
std::string surface_dir(const std::string& id);
bool prepare_surface_dir(const std::string& id, uid_t uid);
constexpr const char* kSurfaceDirInContainer = "/run/facet/surface";

// Capability endpoints: a directory shared between the provider of a
// capability and each module that requires it (e.g. the Wayland socket).
// Host side: <root>/<provider>/<capability>/<consumer>. The provider sees the
// whole <capability> directory, each consumer only its own subdirectory.
std::string endpoint_dir(const std::string& provider, const std::string& capability,
                         const std::string& consumer = {});
// Creates the directory (and parents) owned by `uid` (0 = leave the owner), mode 0755.
bool prepare_endpoint_dir(const std::string& dir, uid_t uid);
constexpr const char* kProvidesInContainer = "/run/facet/provides";
// Where plugins with the "gpu" permission find the installed OpenGL package (read-only).
constexpr const char* kGlInContainer = "/run/facet/gl";
constexpr const char* kRequiresInContainer = "/run/facet/requires";

// Namespaces the kernel offers (/proc/self/ns), detected once. Android and
// old or trimmed-down embedded kernels often have only some, or none.
struct Namespaces {
    bool mnt = false, pid = false, ipc = false, uts = false, net = false;
    bool all() const { return mnt && pid && ipc && uts && net; }
    std::string missing() const;  // "pid, ipc, uts, net"; empty when all are there
};
const Namespaces& namespaces();

// How plugins run:
//  Container  in their own namespaces (those the kernel has) with a private root;
//  User       as plain processes, but each as its own user (no mount namespace);
//  Plain      as Facet's own user: not root or FACET_SANDBOX=0 (development).
enum class Mode { Plain, User, Container };
Mode mode();
// True in Container mode.
bool available();
// Container or User mode with less isolation than a full container. Logged once.
bool reduced();

// Whether a plugin without the "network" permission can be kept off the
// network: a network namespace, or else a seccomp filter that only allows
// Unix sockets.
bool can_deny_network();

// The supplementary groups that give access to the device nodes of the
// granted permissions (cameras, sound, render nodes).
std::vector<gid_t> device_groups(const std::vector<std::string>& granted);

// Child side, right after fork() (async-signal-safe): become `uid` with only
// `groups`, no new privileges and, with `deny_network` (and no network
// namespace), no sockets other than Unix ones. Exits with kSetupFailed on failure.
void drop_privileges(uid_t uid, const std::vector<gid_t>& groups, bool deny_network);

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
// of `pid`) or, without containers or PID namespaces, its process group.
void signal_all(pid_t pid, bool sandboxed, int sig);

}  // namespace facet::plugins::sandbox
