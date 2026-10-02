# Facet architecture

How the core and the plugin model work. The code follows this document; when
behaviour changes, update it here first.

---

## 1. Processes

```
┌──────────────────────── facet (core, 1 process) ───────────────────────┐
│ platform ── output (fbdev / x11 / headless), input (evdev), power      │
│ gfx      ── canvas, path rasterizer, TrueType fonts                    │
│ ui       ── Theme + ui::Context (the single screen API), icons         │
│ i18n     ── translation catalog (English source strings as keys)       │
│ app      ── splash, menu, dashboard, settings, plugin screens          │
│ plugins  ── PluginHost: discovery, launch, IPC, watchdog, restarts     │
└──────┬─────────────────────────┬───────────────────────────────────────┘
       │ stdin/stdout (NDJSON)   │
┌──────▼───────┐          ┌──────▼───────┐
│ plugin A     │          │ plugin B     │   every plugin is its own process
└──────────────┘          └──────────────┘
```

**Every plugin is a separate process.** That is the only honest way to meet
"a plugin failure never takes the panel down": a segfault, a memory leak, a
camera driver hang or an endless loop stay inside the plugin. The core only
sees a broken pipe or silence and restarts it.

Plugins **do not draw pixels**. They send a *description* of their screen (a
widget tree) and the core renders it with the same `ui::Context` the built-in
screens use. That gives every plugin the same design, themes and scaling, and
a plugin can be written in any language that reads and writes JSON lines.

For custom graphics a plugin uses the `canvas` widget: it sends draw
operations with theme colours and touch targets, and the core still renders
them (3.7). A pixel-buffer canvas for video can follow; the widget set is
extensible.

## 2. Core lifecycle

1. `main` → platform (fbdev/x11), console to graphics mode, fonts, display on.
2. **Splash** (`siakinnik.com` + progress). Stages: config → fonts → plugins →
   waiting for the system (`systemctl is-system-running` ≠ `starting`, 90 s max).
3. **Menu** (tiles for built-in screens and plugins).
4. Event-driven main loop: `poll()` on input fds and plugin pipes. The timeout
   is the next due event (minute change, plugin ping, animation). Idle means no
   frames and ~0 % CPU.

Early start: a systemd unit with `DefaultDependencies=no`, after
`local-fs.target`, before `getty@tty1` (`deploy/facet.service.in`).

## 3. Plugin model

### 3.1 Layout on disk

```
<prefix>/lib/facet/plugins/<id>/    installed by scripts/install.sh (prefix /usr/local)
/usr/lib/facet/plugins/<id>/        shipped with a distribution image
/var/lib/facet/plugins/<id>/        installed from the store (stage 3)
$FACET_PLUGIN_PATH                  development: ':'-separated directories
  manifest.json
  <exec>                            executable
/var/lib/facet/data/<id>/           plugin data ($FACET_PLUGIN_DATA)
```

The first directory containing a given plugin id wins.

### 3.2 manifest.json (API 3)

```json
{
  "id": "max",
  "name": "MAX",
  "version": "0.1.0",
  "sdk": "0.4.0-alpha",
  "api": 3,
  "exec": "max",
  "permissions": [
    "network",
    "notifications",
    "background",
    { "name": "camera", "optional": true, "transient": true,
      "reason": { "en": "Video calls", "ru": "Видеозвонки" } },
    { "name": "microphone", "optional": true, "transient": true },
    "wayland.window",
    { "name": "wayland.clipboard", "optional": true }
  ],
  "provides": [],
  "requires": ["web.runtime@1", "display.wayland@1"],
  "tile": { "icon": "chat" }
}
```

- `api` is the protocol and manifest version (`facet::sdk::kApiVersion`).
  Plugins with another value are listed but not started: Settings > Apps shows
  them as incompatible ("made for an older Facet, SDK …") until they are
  updated. There is no compatibility layer.
- `sdk` is written by the build (`facet_plugin_manifest()` in the SDK's
  CMake), so the core can say which SDK an incompatible plugin was built with.
  The plugin also reports it in its `hello`.
- `name`, `tile.title` and `settings.title` are a plain string or an object
  of translations (`en` is the fallback); titles default to `name`.
- `tile` (optional) puts a tile on the home screen. `settings` (optional,
  `true` or `{ "title": … }`) lists the plugin's screen under Settings >
  Modules instead or as well. A plugin may have both, one or none (keyboards).
- `permissions` are requested; the user grants them in Settings > Apps
  (§3.4). An entry is a name (required, persistent) or an object:
  `optional` (the plugin runs without it and must degrade gracefully),
  `transient` (asked for at run time and held only while in use; devices
  only) and `reason` (shown to the user).
- `provides` / `requires` are versioned capabilities, `"name@version"`
  (version 1 when omitted). A plugin whose `requires` no enabled plugin
  `provides` with at least that version is not started ("needs …").
  Keyboards provide `input.keyboard@1`; the core accepts keyboard messages
  only from such plugins.
- `tile.icon` is one of the built-in icons: `clock`, `display`, `camera`,
  `settings`, `warning`, `plugin`, `back`, `chevron`, `shift`, `backspace`,
  `enter`, `gauge`, `chat`, `bell`, `mic`, `phone`, `close`. Unknown names show
  the generic plugin icon, so a plugin may use icons of newer cores. At run time
  a plugin can change its icon (a built-in name or a canvas drawing on a
  24 × 24 grid) and show a badge (e.g. an unread count).

Bundled plugins live in `plugins/` of the core repository, are part of every
core release and are installed and updated together with the core. Today this
is the default keyboard (`plugins/keyboard`).

### 3.3 States

Besides the states below, an enabled plugin can be **blocked** and is then
not started at all: incompatible (`api`), waiting for the user to review its
permissions, or missing a dependency. The home screen shows "Modules need
attention" while any enabled plugin is blocked or failed.

```
          enable                  hello ok
Disabled ───────► Starting ─────────────────► Running
   ▲                 │ timeout/crash            │ crash / no pong / garbage
   │ disable         ▼                          ▼
   └──── Stopping ◄─ Backoff ◄──────────────── (crash)
                     │ ≥5 crashes in 10 min
                     ▼
                   Failed ── "Restart" in Settings ──► Starting
```

| Situation | Core reaction |
|---|---|
| process exited / closed stdout | crash |
| no `hello` within 5 s of start | SIGKILL, crash |
| no `pong` for 15 s (ping every 5 s) | SIGKILL, crash (hung) |
| > 10 invalid lines or a line > 1 MB | SIGKILL, crash (protocol violation) |
| outgoing buffer to the plugin > 1 MB | SIGKILL, crash (not reading stdin) |
| crash | restart after 1, 2, 4 … 60 s |
| 5 crashes in 10 min | `Failed`; the tile shows the error and waits for the user |
| disabled by the user | `shutdown` → 2 s → SIGKILL, not counted as a crash |
| core dies | `PR_SET_PDEATHSIG` terminates plugins with it |

**Fail-safe by default:** whatever a plugin holds (e.g. a switched-off
screen) is released when it dies. If the screen-control plugin crashes, the
screen turns on.

Failure reasons are stored untranslated (message key + arguments + exit
status) and rendered in the current UI language when shown.

### 3.4 Permissions and containers

Permissions have protection levels, as on Android: **normal** ones are
granted without asking (listed, the user can revoke them), **dangerous** ones
are decided by the user before the plugin first starts, **special** ones are
dangerous ones shown with a warning (system-wide powers).

| Permission | Level | Transient | What the plugin gets |
|---|---|---|---|
| (none) | | | `/plugin` (its directory, read-only), `/data` (its data, read-write), `/tmp`, read-only system libraries (`/usr`, `/lib`), `/dev/null` & co, its own `/proc`; no network (own network namespace, loopback only) |
| `network` | dangerous | | the host's network, `/etc/resolv.conf`, `/etc/hosts`, `/etc/ssl` |
| `camera` | dangerous | yes | `/dev/video*`, `/dev/v4l` |
| `microphone` | dangerous | yes | ALSA capture nodes (`/dev/snd/pcmC*D*c`, control, timer) |
| `audio` | normal | | ALSA playback nodes |
| `gpu` | normal | | render nodes `/dev/dri/renderD*` |
| `storage.downloads` | dangerous | | the shared `/shared/Downloads` (`<data>/shared/Downloads`) |
| `system.stats` | dangerous | | read-only host `/proc`, `/sys` and the host's root file system at `/host` |
| `notifications` | dangerous | | may post notifications and calls (§3.6) |
| `background` | normal | | keeps running while none of its screens is open |
| `wake_lock` | normal | | may keep the screen on |
| `display.power` | special | | the core accepts `display` requests and sends `activity` |
| `notifications.distributor` | special | | posts on behalf of other modules / apps, receives copies of all notifications |
| `wayland.compositor` | special | | `/dev/dri/*`, `/dev/input/event*`: runs the display server |
| `wayland.window` | normal | | client scope: may show windows (enforced by the compositor) |
| `wayland.clipboard` | dangerous | | client scope: clipboard |
| `wayland.screencopy` | dangerous | yes | client scope: screen capture |

Choices are stored in the config as `permissions.<id>.<name>` = `allow`,
`ask` (transient only) or `deny`. A plugin is not started while a dangerous
or special permission has no choice yet ("needs permission"), or while a
required (not optional) permission is denied. Changing a persistent
permission restarts the plugin; optional ones it lacks are simply missing
from `hello.permissions` and the plugin must keep working.

**Transient permissions.** The plugin calls `request_permission("camera")`
when it needs the device (e.g. a call starts). With `allow` the core grants at
once, with `ask` it shows a dialog ("Allow this time", "Allow while in use",
"Don't allow"), with `deny` it refuses. Granting creates the device nodes in
the running container (`mknod` in its mount namespace, owned by the plugin's
user); `release_permission()` or the end of the process removes them. The
user can withdraw a grant (indicator in the top right corner → module page →
Stop): the nodes are removed at once and a plugin that does not release
within 5 s is restarted, which closes what it still has open.

**Wayland scopes are per client.** The compositor module (provides
`display.wayland`, permission `wayland.compositor`) has the screen and input
devices; client modules never inherit them. The core tells the compositor
which `wayland.*` scopes each client module was granted (`wayland_client`),
and the compositor enforces them (the plan is one `security-context-v1`
socket per client container).

Every plugin runs in its own mount, PID, IPC and UTS namespaces (and network
namespace without `network`) as its own unprivileged user (uids from 64000,
remembered in the config as `plugin_uids`), with `no_new_privs`. The data
directory is handed over to that user (`chown`, mode 0700). Nothing of the
core's environment is passed in: the plugin gets `FACET_PLUGIN_ID`,
`FACET_PLUGIN_DATA=/data`, `FACET_API`, `HOME=/data`, `TMPDIR=/tmp`.

**Background.** A plugin with a tile or settings page and without
`background` is paused (SIGSTOP of its whole container) 10 s after its
screen closes and resumed when it opens again or one of its notifications is
tapped. Settings > Apps lists plugins running in the background with what
they say they are doing (`begin_background(reason)`), and the Wayland clients
the compositor reports (`wayland_clients`).

Containers need Facet to run as root (the service does). Otherwise, or with
`FACET_SANDBOX=0`, plugins run as plain processes and the UI says that
permissions are not enforced. `FACET_AUTO_GRANT=1` grants everything without
asking (development, tests; `scripts/dev.sh` sets it). Plugins that bring their
own root file system (container images, e.g. a browser) are the next step.

### 3.5 Protocol v3 (NDJSON over stdin/stdout)

One line = one JSON object with a `t` field. The plugin's stderr goes to the
core log prefixed with `[id]`.

Core → plugin:

| `t` | fields | meaning |
|---|---|---|
| `hello` | `api`, `data_dir`, `permissions` (granted), `theme`, `locale`, `timezone`, `content_width` | first message |
| `ping` | `seq` | watchdog, answer with `pong` |
| `visible` | `value: bool` | the plugin screen was opened/closed |
| `event` | `id`, `value`, `action?` | the user changed a widget; `action: "submit"` when "Done" was pressed in a text field |
| `locale` | `value` | UI language changed (`en`, `ru`, …); re-send UI and tile |
| `timezone` | `value` | time zone changed (IANA id, `""` = system); the SDK applies it to `TZ` |
| `layout` | `content_width` | width of the content column in dp changed (canvas width); re-send UI |
| `activity` | — | the screen was touched (`display.power` only) |
| `keyboard_show` | `mode`, `width`, `langs` | open the keyboard (`input.keyboard` only): field mode `text`/`number`, width in dp, layout languages |
| `keyboard_key` | `hit` | a key of the keyboard's drawing was tapped |
| `keyboard_hide` | — | the text field lost focus |
| `permission` | `name`, `granted` | answer to `permission_request`, or a transient grant withdrawn (`granted: false`) |
| `notification_action` | `id`, `action`, `source` | the user tapped (`open`), dismissed (`dismiss`), answered a call (`accept`/`decline`/`timeout`) or pressed a button |
| `notification_posted` | `notification` | copy of every posted notification (subscribed distributors) |
| `wayland_client` | `module`, `scopes`, `running` | compositor only: a client module started/stopped and its scopes |
| `shutdown` | — | exit within 2 s |

Plugin → core:

| `t` | fields | meaning |
|---|---|---|
| `hello` | `api`, `sdk`, `id`, `version` | reply to hello |
| `pong` | `seq` | |
| `ui` | `root` | screen tree (3.7) |
| `tile` | `subtitle` | text under the plugin's menu tile |
| `display` | `on: bool` | desired screen state (`display.power`) |
| `keyboard_ui` | `height`, `ops` | the keyboard's drawing: canvas ops, full screen width (`input.keyboard`) |
| `input` | `action`, `text?` | typed input: `insert` (with `text`), `backspace`, `enter`, `hide` (`input.keyboard`) |
| `badge` | `value` | text on the tile icon (`""` hides) |
| `tile_icon` | `name` or `ops` | built-in icon or a canvas drawing on a 24 × 24 grid |
| `permission_request` | `name`, `reason?` | ask for a transient permission now |
| `permission_release` | `name` | done with a transient permission |
| `notify` | `notification` | post or replace (`notifications`; `source`/`app_name` need `notifications.distributor`) |
| `notify_cancel` | `id`, `source?` | withdraw a notification |
| `notifications_subscribe` | `value` | distributors: receive `notification_posted` |
| `background` | `value`, `reason?` | background work started / ended (shown in Settings > Apps) |
| `wake_lock` | `value` | keep the screen on (`wake_lock`) |
| `wayland_clients` | `clients` | compositor only: the running Wayland clients |

Plugins send already-translated text: they get the language in `hello` and
`locale`. Unknown fields are ignored, so the protocol grows compatibly.

### 3.6 Notifications

A notification has an `id` (posting the same id again replaces it), `title`,
`body`, `kind` (`message`, `call`, `alarm`, `status`), `priority` (`low`,
`normal`, `high`), an optional built-in `icon` and up to three `actions`
(`{id, label}`). New ones appear as a banner on top of any screen for 5 s
(not `status` / `low`) and stay in the list behind the bell in the status bar
(at most 100, in memory). `high` wakes the screen for 10 s.

A `call` covers the screen with Accept / Decline (as on a phone), keeps the
screen on and rings until answered, cancelled or `timeout` (default 45 s).
It is meant for native Facet clients; Wayland apps' calls come through a
distributor.

**Distributors** (`notifications.distributor`) post on behalf of other
modules (`source`) or of apps that are no module (`app_name`, e.g. a Wayland
client's `org.freedesktop.Notifications`), and with
`subscribe_notifications()` receive a copy of every notification posted in
the system, to route it elsewhere. Actions on a notification go to whoever
posted it. Remote push servers chosen by the user are planned on top of this.

### 3.7 UI tree

```json
{ "title": "Screen & camera", "items": [
  { "type": "section", "title": "Now" },
  { "type": "info",    "label": "Camera", "value": "USB Camera", "tone": "good" },
  { "type": "level",   "label": "Motion", "value": 0.12, "text": "12%" },
  { "type": "toggle",  "id": "enabled", "label": "Manage the screen", "value": true },
  { "type": "select",  "id": "cam", "label": "Camera", "options": ["…"], "value": 0 },
  { "type": "stepper", "id": "interval", "label": "Check every", "value": 2,
    "min": 1, "max": 30, "step": 1, "unit": "s" },
  { "type": "time",    "id": "night_start", "label": "Night starts", "value": 1380, "step": 15 },
  { "type": "button",  "id": "rescan", "label": "Find cameras again", "style": "normal" },
  { "type": "text",    "id": "name", "label": "Name", "value": "", "placeholder": "Tap to type",
    "secure": false, "mode": "text", "max": 256 },
  { "type": "note",    "text": "Small explanatory text" },
  { "type": "canvas",  "id": "pad", "height": 360, "ops": [
    { "op": "rrect", "x": 0, "y": 0, "w": 100, "h": 76, "r": 18,
      "color": "surface", "hit": "1", "pressed": "surface_pressed" },
    { "op": "text",  "x": 0, "y": 0, "w": 100, "h": 76, "text": "1",
      "size": 32, "color": "text", "align": "center", "font": "light" }
  ]}
]}
```

`tone`: `normal`, `good`, `warn`, `bad`, `dim`. Button `style`: `normal`,
`primary`, `danger`. The core applies a widget change locally at once (instant
feedback) and sends `event`; the plugin answers with a new tree. The widget
set equals the `ui::Context` API, so built-in screens and plugins look alike.

#### Canvas: custom drawing

`canvas` lets a plugin draw its own UI (PIN pads, gauges, keyboards) while
the core still does all rendering. Cores ignore ops they do not know. It spans the content column
(`content_width` dp, from `hello` and `layout`) and is `height` dp tall; ops
use dp relative to its top-left corner and are clipped to it.

| `op` | fields |
|---|---|
| `rect` | `x`, `y`, `w`, `h`, `color` |
| `rrect` | `x`, `y`, `w`, `h`, `r`, `color` |
| `circle` | `cx`, `cy`, `r`, `color` |
| `ring` | `cx`, `cy`, `r`, `width`, `color` |
| `line` | `x1`, `y1`, `x2`, `y2`, `width`, `color` |
| `arc` | `cx`, `cy`, `r`, `width`, `start`, `sweep` (degrees, 0 = 12 o'clock, clockwise), `color` |
| `poly` | `pts` (`[x0, y0, x1, y1, …]`, filled), `color` |
| `polyline` | `pts`, `width`, `color` |
| `text` | `x`, `y`, `w`, `h`, `text`, `size`, `color`, `align` (`start`/`center`/`end`), `font` (`regular`/`medium`/`light`) |
| `icon` | `x`, `y`, `size`, `name`, `color` |

Colours are theme tokens (`bg`, `surface`, `surface_pressed`, `text`,
`text_dim`, `accent`, `on_accent`, `divider`, `track`, `good`, `warn`, `bad`),
so custom drawing follows the dark/light theme, or `#RRGGBB[AA]`. A `rect`,
`rrect` or `circle` with `hit` is a touch target: the core shows `pressed`
(or a derived colour) while it is held, without a round trip, and sends
`event` with the canvas `id` and the hit id as `value` on tap. Input that must
stay private (PIN) is best drawn this way: it never leaves the plugin.
`examples/pinpad` is a complete example and a template for new plugins.

C++ plugins use `facet_sdk` (`sdk/`): JSON, the message loop
(`facet::sdk::Plugin`), the screen builder (`facet::sdk::Screen`), the canvas
builder (`facet::sdk::Canvas`) and the translation catalog (`plugin.catalog()`,
`plugin.tr()`).

#### Text input and keyboards

A `text` widget shows a field; tapping it gives it focus and opens the
on-screen keyboard at the bottom of the screen. The content above shrinks and
scrolls so the field stays visible. Every change is sent as `event(id, text)`,
"Done" as `event(id, text, action: "submit")` and closes the keyboard; a tap
outside the field and the keyboard closes it too. `mode: "number"` opens a
numeric keypad and accepts digits only.

The keyboard is a plugin (capability `input.keyboard`), chosen in
Settings → Input → Keyboard; the bundled `keyboard` plugin is the default. The
core sends it `keyboard_show`, it answers with `keyboard_ui` (canvas ops); key
taps come back as `keyboard_key` and it replies with `input` actions. Layouts
(EN/RU letters, symbols, shift, numeric pad) are in the SDK
(`facet::sdk::Keyboard`), so a keyboard plugin is a few lines of glue.

Security rules, enforced by the core:

- A keyboard plugin never receives the contents of any field; it only learns
  the keys it draws itself.
- `secure: true` fields (PINs, passwords) are shown as dots and always use the
  core's built-in keyboard; keyboard plugins are not involved at all.
- `input` is accepted only from the keyboard serving the focused field.
- If the keyboard plugin is missing, disabled or crashed, the built-in
  keyboard takes over, so text input always works.

## 4. Screen power

The core makes the final decision:

```
on = no running plugin with display.power
   || that plugin asks for on
   || a touch happened < 5 s ago
```

A touch on a dark screen only wakes it; the tap does not reach the UI. The
`display-power` plugin gets `activity` and decides how long to keep the screen
on after touches, by schedule or by camera.

## 5. UI API

`ui::Context` is an immediate-mode API; a screen is code that runs every frame:

```cpp
if (ui.begin_screen("settings", tr("Settings")) == ui::HeaderHit::Back) go_back();
ui.section(tr("Appearance"));
if (ui.select("theme", tr("Theme"), {tr("Dark"), tr("Light"), tr("Auto")}, theme)) save();
ui.stepper("bright", tr("Brightness"), brightness, 10, 100, 10, "%");
ui.end_screen();
```

Sizes are in dp: `theme.dp(x) = x * scale`, `scale = min(w, h) / 480`.
Colours come only from `Theme::Palette` (dark and light). Widgets take no
colours or sizes, so the design cannot drift.

## 6. Localisation

- Code contains English only; user-visible strings are wrapped in `tr()` and
  the English text is the key (gettext style). `tr("Restart “{}”", {name})`
  fills positional `{}` arguments.
- Translations live only in `i18n/` directories (`src/i18n/ru.cpp` in the
  core, one per plugin). `scripts/check_i18n.py` reports non-English text
  anywhere else (run it before committing, or in CI).
- Language: Settings → Language (saved), otherwise `FACET_LANG` / `LANG`,
  otherwise English. Changing it re-renders the core and sends `locale` to
  plugins.

## 7. Time zone

Settings → Date & time picks a region, then a city, from the system tzdata
(`/usr/share/zoneinfo/zone1970.tab`). Facet sets `TZ` for its own process
instead of changing the system zone: no root needed, no side effects on the
device. Plugins inherit it at start and get `timezone` messages on change;
`facet_sdk` applies them automatically. "System" follows `/etc/localtime`.

## 8. Graphics

Everything is rendered in software into our own XRGB8888 buffer:

- a signed-area accumulation rasterizer (analytic anti-aliasing) shared by
  glyphs, rounded rectangles, circles and icons;
- a TrueType parser (`glyf`, `cmap` 4/12, composite glyphs) with a glyph
  cache per (glyph, px);
- output: `fbdev` (32/16 bpp) on devices, `x11` for development, `headless`
  for scripted tests.

No dependencies besides libc (and libX11 for the dev window). A DRM/KMS
backend fits behind the same `Platform` interface. Known gap: every frame is
redrawn in full, which is slow on 4K panels; dirty-region rendering is next.

## 9. Not in scope yet

- plugin sandboxing (separate uid, seccomp, cgroup limits), after the store;
- plugin signing;
- a pixel-buffer (shared memory) canvas for video and camera views.
