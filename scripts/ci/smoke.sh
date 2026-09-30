#!/usr/bin/env bash
# Smoke test of a built binary: --version, then a short headless run that must
# render the menu. FACET_RUNNER runs foreign-arch binaries (e.g. qemu-aarch64-static).
#   scripts/ci/smoke.sh build/facet
set -euo pipefail
bin="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
runner=(${FACET_RUNNER:-})
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "== --version"
"${runner[@]}" "$bin" --version

echo "== headless run"
cat > "$work/script" <<EOF
wait 4
shot $work/menu.ppm
quit
EOF
FACET_BACKEND=headless FACET_SIZE=800x480 FACET_SKIP_BOOT_WAIT=1 FACET_DATA="$work/data" \
    FACET_SCRIPT="$work/script" timeout 60 "${runner[@]}" "$bin"

size=$(stat -c %s "$work/menu.ppm" 2>/dev/null || echo 0)
expected=$((800 * 480 * 3))
if (( size < expected )); then
    echo "smoke test failed: no screenshot rendered ($size bytes)" >&2
    exit 1
fi
echo "ok: rendered $size bytes"
