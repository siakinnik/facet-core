# Facet

Facet is an open, offline touch-panel shell for smart homes: a full-screen UI
for a wall- or bedside-mounted Linux device. The core is small; everything else
comes as plugins that run as separate processes, so a crashing plugin never
takes the panel down.

- Pure C++20, no UI frameworks. Draws straight to the Linux framebuffer from
  early boot; needs only libc (and libX11 for the optional development window).
- Built-in: splash screen, tile menu, clock dashboard, settings (theme,
  brightness, language, plugins).
- Draws with the processor everywhere; optionally with the graphics card
  (DRM/KMS + OpenGL ES), whose drivers Facet downloads on demand.
- Dark and light themes, automatic day/night switching, English and Russian UI,
  per-panel time zone (without touching the system zone).
- On-screen keyboard as a replaceable plugin (EN/RU, symbols, numeric pad);
  secure fields (PINs, passwords) always use the core's built-in keyboard.
- Status bar with the network state (Wi-Fi with signal and SSID via nl80211,
  Ethernet, offline), read without root.
- Every build knows what it is: version, `dev`/`release` channel, git commit
  and repository, shown on the splash, the menu and in Settings, and by
  `facet --version`.
- Plugins: out-of-process, each in its own container as its own user, with
  Android-like permissions (optional ones, "while in use" device access),
  notifications with banners and incoming calls, badges, background limits;
  own pixels (surfaces, also full screen) with touch and the core's keyboard;
  capabilities shared between modules (e.g. desktop apps through the
  [Wayland module](https://github.com/siakinnik/facet-wayland));
  watchdog-supervised, fail-safe. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Status: **alpha** (0.0.1-alpha).

## Install on a device

Prebuilt static binaries for **x86_64**, **aarch64** (64-bit Raspberry Pi OS)
and **armv7** (32-bit Raspberry Pi OS) are published on the
[Releases](https://github.com/siakinnik/facet-core/releases) page. Nothing to
compile; the device only needs Linux with a framebuffer (`/dev/fb0`) and a
TrueType font.

```bash
sudo apt install fonts-dejavu-core   # any supported font, see "Dependencies"
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash
```

`get.sh` picks the newest release for the device's architecture, verifies its
checksum, installs `/usr/local/bin/facet` and enables `facet.service`, which
starts right after local filesystems are mounted and takes over tty1.

| Command (append to `… \| sudo bash -s --`) | What it does |
|---|---|
| *(nothing)* | install or update to the newest release and start |
| `--version v0.0.1-alpha` | install a specific release |
| `--no-start` | install and enable, start on next boot |
| `--uninstall` | remove binary and service (keeps settings and data) |

Logs: `journalctl -u facet -f`. Update: run the same command again.
Offline: download the archive from Releases and run
`sudo scripts/get.sh --file facet-<version>-linux-<arch>.tar.gz`.

Plugins are installed the same way from their own GitHub releases, e.g. the
screen/camera plugin:

```bash
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash -s -- --plugin siakinnik/facet-display-power
```

<!--
Available plugins:

| Repository | What it does |
|---|---|
| [facet-display-power](https://github.com/siakinnik/facet-display-power) | screen on/off by schedule, touch and camera presence |
| [facet-sysmon](https://github.com/siakinnik/facet-sysmon) | CPU, memory, temperatures, disks, network, top processes |
| [facet-telegram-dm-notif](https://github.com/siakinnik/facet-telegram-dm-notif) | unread messages from private Telegram chats (read only) |
-->

`--remove-plugin <id>` removes one. A plugin release is an archive named
`<repo>-<version>-linux-<arch>.tar.gz` whose top directory holds
`manifest.json` and the executable.

## Build from source

For development or unsupported architectures. Needs a C++20 compiler
(GCC 10+ or Clang 12+); CMake is optional.

```bash
sudo apt install g++ cmake fonts-dejavu-core
git clone https://github.com/siakinnik/facet-core.git
cd facet-core
sudo scripts/install.sh
```

This builds the core and every sibling `facet-*` plugin, installs them under
`/usr/local` and enables `facet.service`.

| Command | What it does |
|---|---|
| `sudo scripts/install.sh` | build + install + enable and start the service |
| `sudo scripts/install.sh ../my-plugin` | install specific plugin sources |
| `sudo scripts/install.sh --no-start` | install and enable, start on next boot |
| `scripts/install.sh --prefix ~/.local --no-service` | no-root install, run by hand |
| `sudo scripts/install.sh --uninstall` | remove binaries and service (keeps settings) |

Logs: `journalctl -u facet -f`. Update: pull and run the installer again.

### Dependencies

| Distribution | Command |
|---|---|
| Debian, Ubuntu, Raspberry Pi OS | `sudo apt install g++ cmake fonts-dejavu-core` |
| Fedora | `sudo dnf install gcc-c++ cmake dejavu-sans-fonts` |
| Arch | `sudo pacman -S gcc cmake ttf-dejavu` |
| Alpine | `sudo apk add g++ cmake linux-headers font-dejavu` |

Fonts are found by file name anywhere under `/usr/share/fonts`,
`/usr/local/share/fonts` or `~/.local/share/fonts`. Supported families, in
order of preference: Open Sans, Noto Sans, Ubuntu, DejaVu Sans, Liberation
Sans. To use your own, put `regular.ttf`, `medium.ttf` and `light.ttf` into
`<prefix>/share/facet/fonts/` or point `FACET_FONT_DIR` at them.

## Configuration

Most settings are changed on the panel itself (gear icon in the menu). The
environment covers hardware specifics; with the service, edit
`/etc/facet/facet.env` and run `sudo systemctl restart facet`.

| Variable | Meaning |
|---|---|
| `FACET_LANG` | UI language until one is picked in Settings (`en`, `ru`); defaults to `LANG` |
| `FACET_TOUCH_TRANSFORM` | touchscreen orientation: any of `swap`, `invx`, `invy` |
| `FACET_SCALE` | UI scale (default `min(width, height) / 480`) |
| `FACET_FB`, `FACET_TTY` | framebuffer (`/dev/fb0`) and console for graphics mode (`/dev/tty0`) |
| `FACET_BACKEND` | `fbdev` (device), `x11` (dev window) or `headless` (tests) |
| `FACET_DATA` | data directory (default `/var/lib/facet`, else `~/.local/share/facet`) |
| `FACET_PLUGIN_PATH` | extra plugin directories, `:` separated |
| `FACET_FONT_DIR` | extra font directory |
| `FACET_DEBUG_INPUT` | log every touch down/up |
| `FACET_DEBUG_FRAMES` | log frame rate and drawing time every 5 s |
| `FACET_GPU_OPS` | `0`: with the GPU, draw the interface on the CPU and only compose on the GPU |

Plugins are loaded from `<prefix>/lib/facet/plugins/<id>/`,
`/usr/lib/facet/plugins`, `/var/lib/facet/plugins` and `FACET_PLUGIN_PATH`.

For a clean boot without kernel text before the splash, add
`quiet loglevel=3 vt.global_cursor_default=0` to the kernel command line.

## Troubleshooting

**Black screen, but the log looks fine.** The console may have blanked the
display before Facet started; Facet unblanks it on start. Check
`cat /sys/class/graphics/fb0/blank` (0 = on).

**Touches do nothing.** Check that the kernel sees a touchscreen:
`grep -A4 -i touch /proc/bus/input/devices`, and look for
`touch: ... touchscreen` in the Facet log. Then run with
`FACET_DEBUG_INPUT=1` to see whether events arrive. Known laptop quirks:

- I2C touchscreens (ELAN, Goodix, ...) exist in ACPI but are not created
  when the firmware is told an unknown OS version. Kernel options such as
  `acpi_osi=! "acpi_osi=Windows 2020"` can cause this; add an OS string the
  firmware knows, e.g. `"acpi_osi=Windows 2015"`.
- The touchscreen is created but sends no interrupts (`grep ELAN /proc/interrupts`
  does not grow on touch): rebind the driver. Facet picks the device up again
  within 5 seconds.

  ```bash
  echo i2c-ELAN0732:00 | sudo tee /sys/bus/i2c/drivers/i2c_hid_acpi/unbind
  echo i2c-ELAN0732:00 | sudo tee /sys/bus/i2c/drivers/i2c_hid_acpi/bind
  ```
- Touches land in the wrong place: set `FACET_TOUCH_TRANSFORM`.

**No font found.** Install one of the fonts listed under Dependencies.

**Permission denied on /dev/fb0 or /dev/input.** Run as root (the service
does) or add the user to the `video` and `input` groups.

## Development

```bash
scripts/dev.sh            # build core + plugins, open an X11 window (WSLg works)
scripts/dev.sh smoke      # headless scripted run, screenshots in shots/
scripts/check_i18n.py     # fails on non-English text outside i18n/ directories
```

`examples/pinpad` is a small plugin (a PIN pad drawn with the canvas widget)
that doubles as a template for new plugins; `examples/showcase` shows the
API 3 features (notifications, an incoming call, the camera "while in use", a
badge, a wake lock, background work). Both are built with the core and loaded
with `FACET_PLUGIN_PATH=build/examples`.

In the X11 window the mouse acts as a finger and `q` quits. The headless
backend replays `FACET_SCRIPT` (`tap x y`, `wait s`, `shot file.ppm`, ...).

### Releases (GitHub Actions)

- **CI** (`.github/workflows/ci.yml`) runs on every push and pull request:
  translation check, builds (with the X11 dev window and static), headless
  smoke test.
- **Release** (`.github/workflows/release.yml`): bump the version in
  `CMakeLists.txt` (`project(VERSION)` and `FACET_VERSION_SUFFIX`), push, then
  Actions → Release → *Run workflow*. It tags `v<version>`, builds static
  binaries for x86_64, aarch64 and armv7 (ARM builds are smoke-tested under
  QEMU), and publishes a release with the archives and `SHA256SUMS`. Versions
  with a suffix (`-alpha`, `-beta`) are marked as pre-releases. Pushing a
  `v<version>` tag by hand triggers the same build.

### Build info

The build records the version, git commit (with `*` / "modified" for
uncommitted changes), the `origin` repository (credentials stripped) and a
channel: `release` when HEAD is exactly at tag `v<version>` with no local
changes, otherwise `dev`. Builds from a tarball without `.git` can pass
`FACET_BUILD_COMMIT`, `FACET_BUILD_REPO` and `FACET_BUILD_CHANNEL` in the
environment. The version itself is set in `CMakeLists.txt` (`project(VERSION)`
plus `FACET_VERSION_SUFFIX`).

### Translations

User-visible strings are written in English and wrapped in `tr()`; the English
text is the lookup key. Translations live only in `i18n/` directories:
`src/i18n/ru.cpp` for the core, and one per plugin. To add a language, copy
`ru.cpp`, translate the right-hand side, register it in `src/i18n/i18n.cpp`
and add it to `languages()`. Plugins get the language through the protocol
and switch at runtime.

### Layout

```
sdk/            facet_sdk: JSON, i18n, plugin protocol client (used by core and plugins)
src/gfx/        canvas, anti-aliased rasterizer, TrueType, UTF-8
src/ui/         themes, ui::Context (the single UI API), icons
src/platform/   fbdev + evdev, x11, headless, backlight
src/plugins/    plugin host: discovery, IPC, watchdog, restarts
plugins/        bundled plugins, shipped with the core (default keyboard)
examples/       example plugins (PIN pad + text fields: canvas and input demo)
src/i18n/       translations of the core UI
src/app/        main loop and screens
deploy/         systemd unit template and environment file
scripts/        installer, dev runner, i18n check
docs/           architecture and plugin protocol
```

Writing a plugin: see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the
`facet-display-power` plugin as a template.
