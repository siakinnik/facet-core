#!/usr/bin/env bash
# Packs a release archive: facet-<version>-linux-<arch>.tar.gz with
#   bin/facet, share/facet/{facet.service.in,facet.env}, LICENSE, README.md
#   scripts/ci/package.sh <binary> <version> <arch> <out-dir>
set -euo pipefail
bin="$1" version="$2" arch="$3" out="$4"
root="$(cd "$(dirname "$0")/../.." && pwd)"
name="facet-$version-linux-$arch"
stage="$(mktemp -d)/$name"

install -Dm755 "$bin" "$stage/bin/facet"
install -Dm644 "$root/deploy/facet.service.in" "$stage/share/facet/facet.service.in"
install -Dm644 "$root/deploy/facet.env" "$stage/share/facet/facet.env"
for f in LICENSE README.md; do
    [[ -f "$root/$f" ]] && install -m644 "$root/$f" "$stage/$f"
done

mkdir -p "$out"
tar -C "$(dirname "$stage")" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz"
