#!/usr/bin/env bash
# Builds the OpenGL package Facet downloads on demand (Settings > Graphics):
# the GPU helper (facet-gpu) and Mesa's drivers with every library they need
# beyond glibc and libstdc++, from Ubuntu 22.04 (glibc 2.35: runs on any newer
# host), plus their licenses and the list of the Ubuntu packages they come
# from (licenses/SOURCES).
#   scripts/build-gl.sh <out-dir> <version> [arch]
# FACET_GL_SOURCES=1 also writes facet-gl-<version>-sources.tar: the Ubuntu
# source packages of all of it (the LGPL parts must be offered with the
# binaries); FACET_GL_SOURCES=only writes just that, e.g. for a release
# published without it (the versions must still be the ones in the archive).
# Runs in an ubuntu:22.04 Docker container (other architectures through
# QEMU), or directly when already on Ubuntu 22.04 as root.
set -euo pipefail
out="$(mkdir -p "$1" && cd "$1" && pwd)"
version="$2"
arch="${3:-$(uname -m)}"
root="$(cd "$(dirname "$0")/.." && pwd)"

case "$arch" in
    x86_64) platform=linux/amd64 triplet=x86_64-linux-gnu ;;
    aarch64) platform=linux/arm64 triplet=aarch64-linux-gnu ;;
    armv7) platform=linux/arm/v7 triplet=arm-linux-gnueabihf ;;
    *) echo "unknown architecture $arch" >&2; exit 1 ;;
esac

if [[ "${FACET_GL_INSIDE:-}" != 1 ]] && ! { grep -q '^VERSION_CODENAME=jammy' /etc/os-release && [[ $(uname -m) == "${arch/armv7/armv7l}" && $EUID == 0 ]]; }; then
    docker run --rm --platform "$platform" -e FACET_GL_INSIDE=1 -e FACET_GL_SOURCES="${FACET_GL_SOURCES:-}" \
        -v "$root:/src:ro" -v "$out:/out" ubuntu:22.04 \
        bash /src/scripts/build-gl.sh /out "$version" "$arch"
    exit
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends g++ cmake ninja-build pkg-config git libdrm-dev libgbm-dev \
    libegl-dev libgles-dev libegl1-mesa-dev libgl1-mesa-dri libegl-mesa0 >/dev/null

work="$(mktemp -d)"
cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFACET_BUILD_EXAMPLES=OFF \
    -DFACET_WITH_X11=OFF -DFACET_GPU_HELPER=ON >/dev/null
cmake --build "$work/build" --target facet-gpu >/dev/null

stage="$work/stage/facet-gl"
mkdir -p "$stage"/{bin,lib/dri,share/glvnd/egl_vendor.d,licenses}
install -m755 "$work/build/facet-gpu" "$stage/bin/"
# The drivers are hard links of one file: copied together, they stay so.
cp -a /usr/lib/$triplet/dri/*_dri.so "$stage/lib/dri/"
cp /usr/share/glvnd/egl_vendor.d/50_mesa.json "$stage/share/glvnd/egl_vendor.d/"
egl_mesa="$(readlink -f /usr/lib/$triplet/libEGL_mesa.so.0)"
cp "$egl_mesa" "$stage/lib/libEGL_mesa.so.0"

# Everything those need, except what every system has (glibc, libstdc++).
skip='^(libc|libm|libdl|libpthread|librt|libresolv|libutil|ld-linux.*|libstdc\+\+|libgcc_s)\.so'
declare -A seen
for f in "$stage/bin/facet-gpu" "$stage/lib/libEGL_mesa.so.0" "$stage"/lib/dri/*_dri.so; do
    ldd "$f" 2>/dev/null | awk '/=> \// {print $1, $3}'
done | sort -u | while read -r soname path; do
    [[ "$soname" =~ $skip ]] && continue
    [[ -e "$stage/lib/$soname" ]] && continue
    cp -L "$path" "$stage/lib/$soname"
    # dpkg knows a file by the path it was installed at: /lib/... on merged-/usr systems.
    real="$(readlink -f "$path")"
    pkg="$( (dpkg -S "$real" || dpkg -S "${real#/usr}") 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    [[ -n "$pkg" ]] && echo "$pkg" >> "$work/packages"
    [[ -n "$pkg" && -f /usr/share/doc/$pkg/copyright ]] && cp /usr/share/doc/$pkg/copyright "$stage/licenses/$pkg.copyright"
done
for pkg in libgl1-mesa-dri libegl-mesa0; do
    echo "$pkg" >> "$work/packages"
    cp /usr/share/doc/$pkg/copyright "$stage/licenses/$pkg.copyright"
done
# Binary package, its version, and the source package it is built from.
sort -u "$work/packages" | while read -r pkg; do
    dpkg-query -W -f='${Package} ${Version} ${source:Package} ${source:Version}\n' "$pkg"
done > "$stage/licenses/SOURCES"
# The copyright files refer to the full license texts by path; they come along.
cp -r /usr/share/common-licenses "$stage/licenses/common-licenses"
install -m644 "$root/LICENSE" "$stage/licenses/facet-gpu.LICENSE"  # the helper: GPL-3.0
mesa="$(dpkg-query -W -f='${Version}' libgl1-mesa-dri)"
echo "$version (Mesa $mesa)" > "$stage/VERSION"

if [[ "${FACET_GL_SOURCES:-}" != only ]]; then
    name="facet-gl-$version-linux-$arch.tar.gz"
    tar -C "$work/stage" -czf "$out/$name" facet-gl
    echo "$out/$name ($(du -h "$out/$name" | cut -f1), unpacked $(du -sh "$stage" | cut -f1))"
fi
if [[ -n "${FACET_GL_SOURCES:-}" ]]; then
    # The exact versions packed above, from Ubuntu's source archive.
    sed -i 's/^# *deb-src /deb-src /' /etc/apt/sources.list
    apt-get update -qq
    mkdir -p "$work/src/facet-gl-$version-sources"
    cp "$stage/licenses/SOURCES" "$work/src/facet-gl-$version-sources/"
    (cd "$work/src/facet-gl-$version-sources" &&
        awk '{print $3 "=" $4}' SOURCES | sort -u | xargs apt-get source --download-only -qq)
    name="facet-gl-$version-sources.tar"
    tar -C "$work/src" -cf "$out/$name" "facet-gl-$version-sources"
    echo "$out/$name ($(du -h "$out/$name" | cut -f1))"
fi
rm -rf "$work"
