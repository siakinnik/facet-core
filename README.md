# Facet

Facet is an open, offline touch-panel shell for smart homes: a full-screen UI
for a wall- or bedside-mounted Linux device. The core is small; everything else
comes as plugins that run as separate processes, so a crashing plugin never
takes the panel down.

- Pure C++20, no UI frameworks. Draws straight to the Linux framebuffer from
  early boot; needs only libc (and libX11 for the optional development window).
- Built-in: splash screen, tile menu, clock dashboard, settings (theme,
  brightness, language, plugins).
- Dark and light themes, automatic day/night switching, English and Russian UI,
  per-panel time zone (without touching the system zone).
- Status bar with the network state (Wi-Fi with signal and SSID via nl80211,
  Ethernet, offline), read without root.
- Every build knows what it is: version, `dev`/`release` channel, git commit
  and repository, shown on the splash, the menu and in Settings, and by
  `facet --version`.
- Plugins: out-of-process, watchdog-supervised, fail-safe. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Status: **alpha** (0.0.1-alpha).

## Install on a device

Requirements: Linux with a framebuffer (`/dev/fb0`), a C++20 compiler
(GCC 10+ or Clang 12+) and a TrueType font. CMake is optional.

```bash
# Debian/Ubuntu example; see "Dependencies" for other distributions
sudo apt install g++ cmake fonts-dejavu-core

git clone <facet-core repo> facet-core
git clone <plugin repo> facet-display-power   # optional: plugins next to the core are picked up
cd facet-core
sudo scripts/install.sh
```

This builds the core and every sibling `facet-*` plugin, installs them under
`/usr/local`, and enables `facet.service`, which starts right after local
filesystems are mounted and takes over tty1.

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

In the X11 window the mouse acts as a finger and `q` quits. The headless
backend replays `FACET_SCRIPT` (`tap x y`, `wait s`, `shot file.ppm`, ...).

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
src/i18n/       translations of the core UI
src/app/        main loop and screens
deploy/         systemd unit template and environment file
scripts/        installer, dev runner, i18n check
docs/           architecture and plugin protocol
```

Writing a plugin: see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the
`facet-display-power` plugin as a template.
