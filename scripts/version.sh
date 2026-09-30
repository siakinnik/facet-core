#!/usr/bin/env bash
# Prints the Facet version from CMakeLists.txt, e.g. "0.0.1-alpha".
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/^project(facet VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
suffix="$(sed -n 's/^set(FACET_VERSION_SUFFIX "\([^"]*\)".*/\1/p' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "version not found in CMakeLists.txt" >&2; exit 1; }
echo "$version${suffix:+-$suffix}"
