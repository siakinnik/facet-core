#!/usr/bin/env bash
# Packs a release archive: facet-<version>-linux-<arch>.tar.gz with
#   bin/facet, lib/facet/plugins/<bundled>/, share/facet/{facet.service.in,facet.env},
#   LICENSE, README.md
#   scripts/ci/package.sh <build-dir> <version> <arch> <out-dir>
set -euo pipefail
build="$1" version="$2" arch="$3" out="$4"
bin="$build/facet"
root="$(cd "$(dirname "$0")/../.." && pwd)"
name="facet-$version-linux-$arch"
stage="$(mktemp -d)/$name"

install -Dm755 "$bin" "$stage/bin/facet"
# Bundled plugins (built into <build>/plugins/<name>/).
for manifest in "$build"/plugins/*/manifest.json; do
    [[ -f "$manifest" ]] || continue
    plugin="$(basename "$(dirname "$manifest")")"
    exec_name="$(sed -n 's/.*"exec"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -n1)"
    install -Dm644 "$manifest" "$stage/lib/facet/plugins/$plugin/manifest.json"
    install -Dm755 "$build/plugins/$plugin/$exec_name" "$stage/lib/facet/plugins/$plugin/$exec_name"
done
install -Dm644 "$root/deploy/facet.service.in" "$stage/share/facet/facet.service.in"
install -Dm644 "$root/deploy/facet.env" "$stage/share/facet/facet.env"
for f in LICENSE README.md; do
    [[ -f "$root/$f" ]] && install -m644 "$root/$f" "$stage/$f"
done

mkdir -p "$out"
tar -C "$(dirname "$stage")" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz"
