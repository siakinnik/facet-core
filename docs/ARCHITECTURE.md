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

Later, a `canvas` widget (shared-memory drawing composited by the core) can
cover plugins with custom graphics; the widget set is extensible.

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

### 3.2 manifest.json

```json
{
  "id": "display-power",
  "name": { "en": "Screen & camera", "ru": "Экран и камера" },
  "version": "0.0.1",
  "api": 1,
  "exec": "display-power",
  "capabilities": ["display.power", "camera"],
  "tile": { "icon": "display" }
}
```

- `name` and `tile.title` are either a plain string or an object of
  translations (`en` is the fallback). `tile.title` defaults to `name`.
- `capabilities` lists what the plugin may do. The core ignores messages the
  plugin has no right to send (e.g. `display` without `display.power`);
  `activity` (touch) events go only to plugins with `display.power`.
- `tile.icon` is one of the built-in icons: `clock`, `display`, `camera`,
  `settings`, `warning`, `plugin`.

### 3.3 States

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

### 3.4 Protocol v1 (NDJSON over stdin/stdout)

One line = one JSON object with a `t` field. The plugin's stderr goes to the
core log prefixed with `[id]`.

Core → plugin:

| `t` | fields | meaning |
|---|---|---|
| `hello` | `api`, `data_dir`, `theme`, `locale`, `timezone` | first message |
| `ping` | `seq` | watchdog, answer with `pong` |
| `visible` | `value: bool` | the plugin screen was opened/closed |
| `event` | `id`, `value` | the user changed a widget |
| `locale` | `value` | UI language changed (`en`, `ru`, …); re-send UI and tile |
| `timezone` | `value` | time zone changed (IANA id, `""` = system); the SDK applies it to `TZ` |
| `activity` | — | the screen was touched (`display.power` only) |
| `shutdown` | — | exit within 2 s |

Plugin → core:

| `t` | fields | meaning |
|---|---|---|
| `hello` | `api`, `id`, `version` | reply to hello |
| `pong` | `seq` | |
| `ui` | `root` | screen tree (3.5) |
| `tile` | `subtitle` | text under the plugin's menu tile |
| `display` | `on: bool` | desired screen state (`display.power`) |

Plugins send already-translated text: they get the language in `hello` and
`locale`. Unknown fields are ignored, so the protocol grows compatibly.

### 3.5 UI tree

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
  { "type": "note",    "text": "Small explanatory text" }
]}
```

`tone`: `normal`, `good`, `warn`, `bad`, `dim`. Button `style`: `normal`,
`primary`, `danger`. The core applies a widget change locally at once (instant
feedback) and sends `event`; the plugin answers with a new tree. The widget
set equals the `ui::Context` API, so built-in screens and plugins look alike.

C++ plugins use `facet_sdk` (`sdk/`): JSON, the message loop
(`facet::sdk::Plugin`), the screen builder (`facet::sdk::Screen`) and the
translation catalog (`plugin.catalog()`, `plugin.tr()`).

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
- the `canvas` widget with shared memory.
