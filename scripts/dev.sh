#!/usr/bin/env bash
# Build core + display-power plugin and run Facet in an X11 window (WSLg or
# desktop Linux). Extra env passes through, e.g. FACET_SIZE=800x480.
#   scripts/dev.sh              # X11 window
#   scripts/dev.sh smoke        # headless run of tests/plugin.script -> shots/
set -euo pipefail
cd "$(dirname "$0")/.."
PLUGIN_DIR=${PLUGIN_DIR:-../facet-display-power}

cmake -S . -B build -G Ninja >/dev/null && cmake --build build
cmake -S "$PLUGIN_DIR" -B "$PLUGIN_DIR/build" -G Ninja >/dev/null && cmake --build "$PLUGIN_DIR/build"

export FACET_DATA=${FACET_DATA:-$PWD/.dev-data}
export FACET_PLUGIN_PATH=$PLUGIN_DIR/build
export FACET_SKIP_BOOT_WAIT=1
export FACET_AUTO_GRANT=${FACET_AUTO_GRANT:-1}  # development: grant requested permissions

if [[ "${1:-}" == "smoke" ]]; then
    mkdir -p shots
    FACET_BACKEND=headless FACET_SCRIPT=tests/plugin.script exec ./build/facet
fi
exec ./build/facet
