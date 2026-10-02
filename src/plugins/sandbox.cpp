#include "plugins/sandbox.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/config.h"

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442  // same number on every architecture
#endif
#ifndef SYS_pivot_root
#error "pivot_root syscall number unknown"
#endif

namespace facet::plugins::sandbox {

namespace {

// Kernel ABI of mount_setattr(2); defined here because older headers lack it.
struct MountAttr {
    uint64_t attr_set, attr_clr, propagation, userns_fd;
};
constexpr uint64_t kAttrRdonly = 0x00000001;
constexpr uint64_t kAttrNosuid = 0x00000002;
constexpr unsigned kAtRecursive = 0x8000;

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool is_dir(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_symlink(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string stage_dir(const std::string& id) {
    struct stat st;
    std::string base = stat("/run", &st) == 0 ? "/run/facet/sandbox" : paths::data_root() + "/sandbox";
    return base + "/" + id;
}

// One step of building the container's root, prepared before clone() so the
// child only makes system calls.
struct Op {
    enum Kind { Dir, File, Tmpfs, Bind, Proc, Devpts, Symlink, Write } kind;
    std::string src, dst;
    bool ro = true, rec = false;
    std::string data;  // tmpfs options or file contents

    Op(Kind k, std::string s, std::string d, bool read_only = true, bool recursive = false, std::string extra = {})
        : kind(k), src(std::move(s)), dst(std::move(d)), ro(read_only), rec(recursive), data(std::move(extra)) {}
};

// Appends what a plain bind of a host path needs, mirroring symlinks.
void add_host_path(std::vector<Op>& ops, const std::string& stage, const std::string& path, bool ro = true) {
    if (is_symlink(path)) {
        char target[PATH_MAX];
        ssize_t n = readlink(path.c_str(), target, sizeof target - 1);
        if (n > 0) {
            target[n] = 0;
            ops.push_back({Op::Symlink, target, stage + path});
        }
    } else if (is_dir(path)) {
        ops.push_back({Op::Dir, {}, stage + path});
        ops.push_back({Op::Bind, path, stage + path, ro, true});
    } else if (exists(path)) {
        ops.push_back({Op::File, {}, stage + path});
        ops.push_back({Op::Bind, path, stage + path, ro, false});
    }
}

// --- Child side: async-signal-safe helpers only (we are a fork of a threaded process).

[[noreturn]] void fail(const char* what, const char* arg) {
    int e = errno;
    auto put = [](const char* s) { (void)!write(2, s, strlen(s)); };
    put("facet sandbox: ");
    put(what);
    if (arg) {
        put(" ");
        put(arg);
    }
    put(": ");
    char num[16];
    int i = 15;
    num[i] = 0;
    do num[--i] = char('0' + e % 10), e /= 10;
    while (e && i > 0);
    put("errno ");
    put(num + i);
    put("\n");
    _exit(kSetupFailed);
}

void make_ro(const char* path, bool rec) {
    MountAttr a{kAttrRdonly | kAttrNosuid, 0, 0, 0};
    if (syscall(SYS_mount_setattr, AT_FDCWD, path, rec ? kAtRecursive : 0u, &a, sizeof a) == 0) return;
    // Kernels before 5.12: only the top mount becomes read-only.
    if (mount(nullptr, path, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID, nullptr) != 0)
        fail("read-only remount", path);
}

void run_op(const Op& op) {
    const char* dst = op.dst.c_str();
    switch (op.kind) {
        case Op::Dir:
            if (mkdir(dst, 0755) != 0 && errno != EEXIST) fail("mkdir", dst);
            break;
        case Op::File: {
            int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
            if (fd < 0) fail("create", dst);
            close(fd);
            break;
        }
        case Op::Tmpfs:
            if (mount("tmpfs", dst, "tmpfs", MS_NOSUID, op.data.c_str()) != 0) fail("tmpfs", dst);
            break;
        case Op::Bind:
            if (mount(op.src.c_str(), dst, nullptr, MS_BIND | (op.rec ? MS_REC : 0), nullptr) != 0) fail("bind", dst);
            if (op.ro) make_ro(dst, op.rec);
            break;
        case Op::Proc:
            if (mount("proc", dst, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) fail("proc", dst);
            break;
        case Op::Devpts:
            if (mount("devpts", dst, "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620") != 0)
                fail("devpts", dst);
            break;
        case Op::Symlink:
            if (symlink(op.src.c_str(), dst) != 0 && errno != EEXIST) fail("symlink", dst);
            break;
        case Op::Write: {
            int fd = open(dst, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0 || write(fd, op.data.data(), op.data.size()) < 0) fail("write", dst);
            close(fd);
            break;
        }
    }
}

uid_t g_chown_uid = 0;  // nftw() callbacks take no context

int chown_entry(const char* path, const struct stat*, int, FTW*) {
    if (lchown(path, g_chown_uid, g_chown_uid) != 0) {
        // Best effort: an unreadable leftover only matters to the plugin itself.
    }
    return 0;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool ends_with(const std::string& s, char c) { return !s.empty() && s.back() == c; }

std::vector<std::string> list_dev(const std::string& dir, bool (*want)(const std::string&)) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name, path = dir + "/" + name;
            struct stat st;
            if (name[0] == '.' || stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode) || !want(name)) continue;
            out.push_back(path);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string shared_downloads() { return paths::data_root() + "/shared/Downloads"; }

}  // namespace

std::string surface_dir(const std::string& id) {
    // tmpfs, so frames never touch a disk: /run as root, /dev/shm otherwise.
    if (getuid() == 0) return "/run/facet/surfaces/" + id;
    return "/dev/shm/facet-" + std::to_string(getuid()) + "/" + id;
}

bool prepare_surface_dir(const std::string& id, uid_t uid) {
    std::string dir = surface_dir(id);
    if (!paths::mkdirs(dir)) return false;
    if (DIR* d = opendir(dir.c_str())) {  // leftovers of the previous run
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') unlink((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    if (uid != 0 && chown(dir.c_str(), uid, uid) != 0) return false;
    return chmod(dir.c_str(), 0700) == 0;
}

std::string endpoint_dir(const std::string& provider, const std::string& capability,
                         const std::string& consumer) {
    std::string root = getuid() == 0 ? "/run/facet/endpoints/"
                                     : "/dev/shm/facet-" + std::to_string(getuid()) + "/endpoints/";
    std::string dir = root + provider + "/" + capability;
    return consumer.empty() ? dir : dir + "/" + consumer;
}

bool prepare_endpoint_dir(const std::string& dir, uid_t uid) {
    if (!paths::mkdirs(dir)) return false;
    if (uid != 0 && chown(dir.c_str(), uid, uid) != 0) return false;
    return chmod(dir.c_str(), 0755) == 0;
}

std::vector<std::string> device_nodes(const std::string& permission) {
    if (permission == kCamera) return list_dev("/dev", [](const std::string& n) { return starts_with(n, "video"); });
    // ALSA: pcmC<card>D<dev>c records, ...p plays; both need the control and timer nodes.
    if (permission == kMicrophone)
        return list_dev("/dev/snd", [](const std::string& n) {
            return (starts_with(n, "pcmC") && ends_with(n, 'c')) || starts_with(n, "control") || n == "timer";
        });
    if (permission == kAudio)
        return list_dev("/dev/snd", [](const std::string& n) {
            return (starts_with(n, "pcmC") && ends_with(n, 'p')) || starts_with(n, "control") || n == "timer";
        });
    if (permission == kGpu)
        return list_dev("/dev/dri", [](const std::string& n) { return starts_with(n, "renderD"); });
    return {};
}

void signal_all(pid_t pid, bool sandboxed, int sig) {
    if (pid <= 0) return;
    if (!sandboxed) {
        ::kill(-pid, sig);  // the plugin leads its own process group
        return;
    }
    char want[64] = {};
    std::string ns = "/proc/" + std::to_string(pid) + "/ns/pid";
    if (readlink(ns.c_str(), want, sizeof want - 1) <= 0) {
        ::kill(pid, sig);
        return;
    }
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            char link[64] = {};
            std::string p = std::string("/proc/") + e->d_name + "/ns/pid";
            if (readlink(p.c_str(), link, sizeof link - 1) > 0 && std::strcmp(link, want) == 0)
                ::kill(pid_t(std::atoi(e->d_name)), sig);
        }
        closedir(d);
    }
}

namespace {

// Runs `fn` in a child that has joined the mount namespace of `pid`
// (setns needs a single-threaded caller). Returns an English error.
template <typename Fn>
std::string in_container(pid_t pid, Fn fn) {
    std::string path = "/proc/" + std::to_string(pid) + "/ns/mnt";
    int ns = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (ns < 0) return "cannot open " + path;
    pid_t child = fork();
    if (child == 0) {
        if (setns(ns, CLONE_NEWNS) != 0) _exit(2);
        _exit(fn() ? 0 : 3);
    }
    close(ns);
    if (child < 0) return "fork failed";
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return "container update failed (" + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + ")";
    return {};
}

}  // namespace

std::string attach_devices(pid_t pid, uid_t uid, const std::string& permission) {
    struct Node {
        std::string path;
        dev_t rdev;
    };
    std::vector<Node> nodes;
    for (const auto& path : device_nodes(permission)) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) nodes.push_back({path, st.st_rdev});
    }
    if (nodes.empty()) return {};  // nothing to give (no such device): the plugin will find none
    return in_container(pid, [&] {
        for (const auto& n : nodes) {
            std::string dir = n.path.substr(0, n.path.rfind('/'));
            mkdir(dir.c_str(), 0755);
            unlink(n.path.c_str());
            // Only the plugin's user may use it; no group membership needed.
            if (mknod(n.path.c_str(), S_IFCHR | 0600, n.rdev) != 0) return false;
            if (chown(n.path.c_str(), uid, uid) != 0) return false;
        }
        return true;
    });
}

std::string detach_devices(pid_t pid, const std::string& permission) {
    std::vector<std::string> nodes = device_nodes(permission);
    if (nodes.empty()) return {};
    return in_container(pid, [&] {
        for (const auto& n : nodes) unlink(n.c_str());  // open handles end when the plugin closes them
        return true;
    });
}

bool available() {
    static const bool ok = [] {
        const char* env = getenv("FACET_SANDBOX");
        return getuid() == 0 && !(env && std::strcmp(env, "0") == 0);
    }();
    return ok;
}

std::string prepare(const Spec& spec) {
    if (!paths::mkdirs(spec.data_dir)) return "cannot create " + spec.data_dir;
    struct stat st;
    if (stat(spec.data_dir.c_str(), &st) != 0) return "cannot stat " + spec.data_dir;
    if (st.st_uid != spec.uid || st.st_gid != spec.uid) {
        // First start in a container (or a new uid): hand the data over.
        g_chown_uid = spec.uid;
        nftw(spec.data_dir.c_str(), chown_entry, 16, FTW_PHYS);
    }
    chmod(spec.data_dir.c_str(), 0700);
    if (has(spec.granted, kDownloads)) {
        // Shared by every plugin allowed to use it: sticky and world-writable, like /tmp.
        if (!paths::mkdirs(shared_downloads())) return "cannot create " + shared_downloads();
        chmod(shared_downloads().c_str(), 01777);
    }
    if (!paths::mkdirs(stage_dir(spec.id))) return "cannot create " + stage_dir(spec.id);
    return {};
}

pid_t spawn(const Spec& spec, int stdin_fd, int stdout_fd, int stderr_fd) {
    const std::string stage = stage_dir(spec.id);
    const bool camera = has(spec.granted, kCamera);
    const bool downloads = has(spec.granted, kDownloads);
    const bool network = has(spec.granted, kNetwork);
    const bool stats = has(spec.granted, kSystemStats);
    const std::string uid = std::to_string(spec.uid);

    std::vector<Op> ops;
    ops.push_back({Op::Tmpfs, {}, stage, false, false, "mode=0755,size=4m"});
    ops.push_back({Op::Dir, {}, stage + "/plugin"});
    ops.push_back({Op::Bind, spec.plugin_dir, stage + "/plugin", true, false});
    ops.push_back({Op::Dir, {}, stage + "/data"});
    ops.push_back({Op::Bind, spec.data_dir, stage + "/data", false, false});
    if (!spec.surface_dir.empty()) {
        ops.push_back({Op::Dir, {}, stage + "/run"});
        ops.push_back({Op::Dir, {}, stage + "/run/facet"});
        ops.push_back({Op::Dir, {}, stage + kSurfaceDirInContainer});
        ops.push_back({Op::Bind, spec.surface_dir, stage + kSurfaceDirInContainer, false, false});
    }
    for (const auto& b : spec.binds) {
        // Every missing parent inside the container's tmpfs root.
        for (size_t at = b.inside.find('/', 1); at != std::string::npos; at = b.inside.find('/', at + 1))
            ops.push_back({Op::Dir, {}, stage + b.inside.substr(0, at)});
        ops.push_back({Op::Dir, {}, stage + b.inside});
        ops.push_back({Op::Bind, b.host, stage + b.inside, false, false});
    }
    ops.push_back({Op::Dir, {}, stage + "/tmp"});
    ops.push_back({Op::Tmpfs, {}, stage + "/tmp", false, false, "mode=0700,size=64m,uid=" + uid + ",gid=" + uid});

    // Read-only system libraries, so dynamically linked plugins work too.
    for (const char* d : {"/usr", "/lib", "/lib32", "/lib64", "/bin", "/sbin"}) add_host_path(ops, stage, d);

    ops.push_back({Op::Dir, {}, stage + "/etc"});
    add_host_path(ops, stage, "/etc/ld.so.cache");
    add_host_path(ops, stage, "/etc/fonts");  // font configuration, for plugins that run GUI apps
    std::string group = "plugin:x:" + uid + ":\n";
    std::vector<gid_t> groups;

    // Devices: the harmless basics, plus cameras when granted.
    ops.push_back({Op::Dir, {}, stage + "/dev"});
    ops.push_back({Op::Tmpfs, {}, stage + "/dev", false, false, "mode=0755,size=64k"});
    for (const char* d : {"/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom"}) {
        ops.push_back({Op::File, {}, stage + d});
        ops.push_back({Op::Bind, d, stage + d, false, false});
    }
    ops.push_back({Op::Symlink, "/proc/self/fd", stage + "/dev/fd"});
    ops.push_back({Op::Symlink, "/proc/self/fd/0", stage + "/dev/stdin"});
    ops.push_back({Op::Symlink, "/proc/self/fd/1", stage + "/dev/stdout"});
    ops.push_back({Op::Symlink, "/proc/self/fd/2", stage + "/dev/stderr"});
    // Pseudo-terminals (terminal apps, ssh): a private instance that only
    // sees the container's own terminals.
    ops.push_back({Op::Dir, {}, stage + "/dev/pts"});
    ops.push_back({Op::Devpts, {}, stage + "/dev/pts"});
    ops.push_back({Op::Symlink, "pts/ptmx", stage + "/dev/ptmx"});
    ops.push_back({Op::Dir, {}, stage + "/dev/shm"});
    ops.push_back({Op::Tmpfs, {}, stage + "/dev/shm", false, false, "mode=0700,size=64m,uid=" + uid + ",gid=" + uid});
    // Device nodes of granted permissions, with their groups.
    for (const auto& perm : spec.granted) {
        for (const auto& path : device_nodes(perm)) {
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            std::string dir = path.substr(0, path.rfind('/'));
            if (dir != "/dev") ops.push_back({Op::Dir, {}, stage + dir});
            ops.push_back({Op::File, {}, stage + path});
            ops.push_back({Op::Bind, path, stage + path, false, false});
            if (std::find(groups.begin(), groups.end(), st.st_gid) == groups.end()) {
                groups.push_back(st.st_gid);
                group += "dev" + std::to_string(st.st_gid) + ":x:" + std::to_string(st.st_gid) + ":plugin\n";
            }
        }
    }
    if (camera) add_host_path(ops, stage, "/dev/v4l");
    if (downloads) {
        ops.push_back({Op::Dir, {}, stage + "/shared"});
        ops.push_back({Op::Dir, {}, stage + "/shared/Downloads"});
        ops.push_back({Op::Bind, shared_downloads(), stage + "/shared/Downloads", false, false});
    }

    if (network) {
        // resolv.conf is often a symlink into /run (systemd-resolved): bind its target.
        char real[PATH_MAX];
        if (realpath("/etc/resolv.conf", real)) {
            ops.push_back({Op::File, {}, stage + "/etc/resolv.conf"});
            ops.push_back({Op::Bind, real, stage + "/etc/resolv.conf", true, false});
        }
        add_host_path(ops, stage, "/etc/hosts");
        add_host_path(ops, stage, "/etc/ssl");
        add_host_path(ops, stage, "/etc/ca-certificates");
    }
    ops.push_back({Op::Write, {}, stage + "/etc/passwd", true, false, "plugin:x:" + uid + ":" + uid + "::/data:/bin/false\n"});
    ops.push_back({Op::Write, {}, stage + "/etc/group", true, false, group});
    ops.push_back({Op::Write, {}, stage + "/etc/nsswitch.conf", true, false, "passwd: files\ngroup: files\nhosts: files dns\n"});
    ops.push_back({Op::Write, {}, stage + "/etc/hostname", true, false, "facet\n"});

    ops.push_back({Op::Dir, {}, stage + "/proc"});
    if (stats) {
        // Host-wide view, read-only: every process, sensors and file systems.
        ops.push_back({Op::Bind, "/proc", stage + "/proc", true, true});
        ops.push_back({Op::Dir, {}, stage + "/sys"});
        ops.push_back({Op::Bind, "/sys", stage + "/sys", true, true});
        ops.push_back({Op::Dir, {}, stage + "/host"});
        ops.push_back({Op::Bind, "/", stage + "/host", true, true});
    } else {
        ops.push_back({Op::Proc, {}, stage + "/proc"});
    }

    std::string exe = "/plugin/" + spec.exec;
    std::vector<std::string> env_store = spec.env;
    std::vector<char*> envp;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);
    char* argv[] = {exe.data(), nullptr};
    const std::string old_root = stage + "/.oldroot";
    const uid_t id = spec.uid;

    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD;
    if (!network) flags |= CLONE_NEWNET;
    // fork() semantics (no new stack) with namespaces: the child is PID 1 of its namespace.
    pid_t pid = pid_t(syscall(SYS_clone, flags, nullptr, nullptr, nullptr, nullptr));
    if (pid != 0) return pid;

    // ---- child
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // PID 1 of a namespace ignores SIGTERM without a handler
    dup2(stdin_fd, 0);
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);

    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) fail("make mounts private", nullptr);
    for (const Op& op : ops) run_op(op);

    if (chdir(stage.c_str()) != 0) fail("chdir", stage.c_str());
    if (mkdir(old_root.c_str(), 0700) != 0) fail("mkdir", old_root.c_str());
    if (syscall(SYS_pivot_root, ".", ".oldroot") != 0) fail("pivot_root", nullptr);
    if (chdir("/") != 0) fail("chdir", "/");
    if (umount2("/.oldroot", MNT_DETACH) != 0) fail("detach old root", nullptr);
    rmdir("/.oldroot");
    if (mount(nullptr, "/", nullptr, MS_REMOUNT | MS_RDONLY | MS_NOSUID, nullptr) != 0) fail("read-only root", nullptr);
    if (sethostname("facet", 5) != 0) {
        // Cosmetic only: the plugin works with any host name.
    }

    // Drop root for good: own user, only the groups of granted devices.
    if (setgroups(groups.size(), groups.data()) != 0) fail("setgroups", nullptr);
    if (setresgid(id, id, id) != 0) fail("setresgid", nullptr);
    if (setresuid(id, id, id) != 0) fail("setresuid", nullptr);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) fail("no_new_privs", nullptr);
    if (chdir("/plugin") != 0) fail("chdir", "/plugin");
    execve(argv[0], argv, envp.data());
    fail("exec", argv[0]);
}

}  // namespace facet::plugins::sandbox
