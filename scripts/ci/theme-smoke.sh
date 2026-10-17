#!/usr/bin/env bash
# Theme packs: runs the core headless with the example theme (a module that is
# only data) chosen, and checks that the pack is found, loads without a
# complaint, starts nothing, and that the screen differs from the plain one.
#   scripts/ci/theme-smoke.sh <build dir>
set -euo pipefail
build="$(cd "$1" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

run() {  # run <name> <config json>: a shot of the menu
    mkdir -p "$work/$1-data"
    printf '%s' "$2" > "$work/$1-data/config.json"
    printf 'wait 3\nshot %s/%s.ppm\nquit\n' "$work" "$1" > "$work/$1.script"
    FACET_BACKEND=headless FACET_SIZE=1280x800 FACET_SCALE=1.6667 FACET_SKIP_BOOT_WAIT=1 FACET_LANG=en \
        FACET_DATA="$work/$1-data" FACET_PLUGIN_PATH="$build/examples" FACET_SCRIPT="$work/$1.script" \
        timeout 60 "$build/facet" > "$work/$1.log" 2>&1 || true
}
run plain '{}'
run themed '{"theme_pack":"example-theme/aurora","theme_anim":2}'

grep -q "found example-theme" "$work/themed.log" || { echo "the theme module was not found" >&2; exit 1; }
if grep -qE "theme:|crashed|failed" "$work/themed.log"; then
    echo "the theme did not load cleanly:" >&2
    grep -E "theme:|crashed|failed" "$work/themed.log" >&2
    exit 1
fi
if grep -q "example-theme ready\|started example-theme" "$work/themed.log"; then
    echo "a data-only module must not be started" >&2
    exit 1
fi
[[ -s "$work/plain.ppm" && -s "$work/themed.ppm" ]] || { echo "no screenshots" >&2; exit 1; }
if cmp -s "$work/plain.ppm" "$work/themed.ppm"; then
    echo "the themed screen is the same as the plain one" >&2
    exit 1
fi
echo "ok: theme pack loaded and drawn"
