#!/usr/bin/env bash
# End-to-end check of the plugin SDK: runs the core headless with the example
# PIN pad plugin, opens it, types digits on its canvas keypad and verifies the
# plugin started and the screen rendered.
#   scripts/ci/plugin-smoke.sh <build dir>
set -euo pipefail
build="$(cd "$1" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# 1280x800 at scale 1.6667: second menu tile, then keys 1 2 3 4.
cat > "$work/script" <<EOF
wait 3
tap 943 446
wait 1.5
tap 443 402
wait 0.3
tap 640 402
wait 0.3
tap 836 402
wait 0.3
tap 443 555
wait 1
shot $work/pinpad.ppm
quit
EOF
FACET_BACKEND=headless FACET_SIZE=1280x800 FACET_SCALE=1.6667 FACET_SKIP_BOOT_WAIT=1 FACET_LANG=en \
    FACET_DATA="$work/data" FACET_PLUGIN_PATH="$build/examples" FACET_SCRIPT="$work/script" \
    timeout 60 "$build/facet" 2>&1 | tee "$work/log"

grep -q "example-pinpad ready" "$work/log" || { echo "plugin did not start" >&2; exit 1; }
if grep -qE "example-pinpad crashed|failed permanently" "$work/log"; then
    echo "plugin crashed" >&2
    exit 1
fi
size=$(stat -c %s "$work/pinpad.ppm" 2>/dev/null || echo 0)
(( size >= 1280 * 800 * 3 )) || { echo "no screenshot rendered" >&2; exit 1; }
echo "ok: example plugin ran and rendered"
