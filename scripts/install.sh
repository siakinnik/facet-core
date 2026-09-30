#!/usr/bin/env bash
# Facet installer: builds the core and plugins from source, installs them and
# (optionally) sets up the systemd service. Works with or without CMake.
#
#   sudo scripts/install.sh                         core + plugins found next to it, service enabled
#   sudo scripts/install.sh ../my-plugin            also build and install extra plugin sources
#   scripts/install.sh --prefix ~/.local --no-service   no root needed
#   sudo scripts/install.sh --uninstall             remove binaries and service (keeps data)
set -euo pipefail

PREFIX=/usr/local
SERVICE=1
START=1
UNINSTALL=0
PLUGINS=()

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --no-service) SERVICE=0; shift ;;
        --no-start) START=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage; exit 2 ;;
        *) PLUGINS+=("$(cd "$1" && pwd)"); shift ;;
    esac
done

CORE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${FACET_BUILD_DIR:-$CORE/build-release}"
JOBS="$(nproc 2>/dev/null || echo 2)"
UNIT=/etc/systemd/system/facet.service
PLUGIN_ROOT="$PREFIX/lib/facet/plugins"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
has_systemd() { [[ -d /run/systemd/system ]] && have systemctl; }

pkg_hint() {
    if have apt-get; then echo "sudo apt install $1"
    elif have dnf; then echo "sudo dnf install $2"
    elif have pacman; then echo "sudo pacman -S $3"
    elif have apk; then echo "sudo apk add $4"
    else echo "install: $1"; fi
}

need_root() {
    if [[ $EUID -ne 0 ]]; then
        die "this needs root (installing into $PREFIX$([[ $SERVICE == 1 ]] && echo ' and a systemd service')). Re-run with sudo, or use --prefix ~/.local --no-service"
    fi
}

# ---------------------------------------------------------------- uninstall

if [[ $UNINSTALL == 1 ]]; then
    [[ -w "$PREFIX" && ! -e $UNIT ]] || need_root
    if has_systemd && [[ -e $UNIT ]]; then
        say "Stopping and removing the service"
        systemctl disable --now facet.service || true
        rm -f "$UNIT"
        systemctl daemon-reload
    fi
    say "Removing $PREFIX/bin/facet and $PLUGIN_ROOT"
    rm -f "$PREFIX/bin/facet"
    rm -rf "$PREFIX/lib/facet"
    echo "Kept settings: /etc/facet and /var/lib/facet (delete them by hand if you want)."
    exit 0
fi

# ---------------------------------------------------------------- checks

mkdir -p "$PREFIX" 2>/dev/null || true
if [[ $SERVICE == 1 || ! -w "$PREFIX" ]]; then need_root; fi
[[ "$(uname -s)" == Linux ]] || die "Facet runs on Linux only"

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    if have g++; then CXX=g++; elif have clang++; then CXX=clang++; elif have c++; then CXX=c++; fi
fi
[[ -n "$CXX" ]] || die "no C++ compiler. Install one: $(pkg_hint g++ gcc-c++ gcc g++)"
echo 'int main(){ auto f = [](auto x) consteval { return x; }; return f(0); }' |
    "$CXX" -std=c++20 -x c++ - -o /dev/null 2>/dev/null ||
    die "$CXX does not support C++20 (need GCC 10+ or Clang 12+)"

USE_CMAKE=0
have cmake && [[ -z "${FACET_NO_CMAKE:-}" ]] && USE_CMAKE=1  # FACET_NO_CMAKE=1 forces the plain compiler path
GENERATOR=()
have ninja && GENERATOR=(-G Ninja)

# Plugins: explicit arguments, else every sibling facet-* project with a manifest.
# Explicit plugins must build; found ones are skipped with a warning if they
# do not (e.g. facet-telegram without TDLib installed).
EXPLICIT_PLUGINS=$(( ${#PLUGINS[@]} > 0 ))
if [[ ${#PLUGINS[@]} -eq 0 ]]; then
    for d in "$CORE"/../facet-*/; do
        [[ -f "$d/manifest.json" ]] || continue  # also skips the unexpanded pattern
        d="$(cd "$d" && pwd)"
        [[ "$d" != "$CORE" ]] && PLUGINS+=("$d")
    done
    [[ ${#PLUGINS[@]} -eq 0 ]] && echo "No plugins found next to $CORE (installing the core only)."
    true
fi

json_field() {  # json_field <file> <key>: first top-level string value of "key"
    sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$1" | head -n1
}

# ---------------------------------------------------------------- build

# Mirrors cmake/build_info.cmake for builds without CMake.
gen_build_info() {  # gen_build_info <version> <out-header>
    local version="$1" out="$2" commit="" dirty=0 repo="" tag="" channel=dev
    if have git && git -C "$CORE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        commit="$(git -C "$CORE" rev-parse --short=12 HEAD 2>/dev/null || true)"
        [[ -n "$(git -C "$CORE" status --porcelain 2>/dev/null)" ]] && dirty=1
        repo="$(git -C "$CORE" config --get remote.origin.url 2>/dev/null || true)"
        tag="$(git -C "$CORE" describe --exact-match --tags HEAD 2>/dev/null || true)"
    fi
    commit="${FACET_BUILD_COMMIT:-$commit}"
    repo="${FACET_BUILD_REPO:-$repo}"
    repo="$(printf '%s' "$repo" | sed -E 's#^[a-z+]+://([^@/]*@)?##; s#^git@([^:]+):#\1/#; s#\.git$##')"
    if [[ -n "${FACET_BUILD_CHANNEL:-}" ]]; then channel="$FACET_BUILD_CHANNEL"
    elif [[ "$tag" == "v$version" && $dirty == 0 ]]; then channel=release; fi
    {
        echo "// Generated by scripts/install.sh. Do not edit."
        echo "#pragma once"
        echo "#define FACET_BUILD_VERSION \"$version\""
        echo "#define FACET_BUILD_CHANNEL \"$channel\""
        echo "#define FACET_BUILD_COMMIT \"$commit\""
        echo "#define FACET_BUILD_DIRTY $dirty"
        echo "#define FACET_BUILD_REPO \"$repo\""
        echo "#define FACET_BUILD_DATE \"$(date -u +%Y-%m-%d)\""
    } > "$out"
}

build_core() {
    say "Building Facet core ($([[ $USE_CMAKE == 1 ]] && echo CMake || echo "$CXX, no CMake found"))"
    if [[ $USE_CMAKE == 1 ]]; then
        cmake -S "$CORE" -B "$BUILD" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_CXX_COMPILER="$(command -v "$CXX")" >/dev/null
        cmake --build "$BUILD" -j "$JOBS"
    else
        local version suffix
        version="$(sed -n 's/^project(facet VERSION \([0-9.]*\).*/\1/p' "$CORE/CMakeLists.txt")"
        suffix="$(sed -n 's/^set(FACET_VERSION_SUFFIX "\([^"]*\)".*/\1/p' "$CORE/CMakeLists.txt")"
        [[ -n "$suffix" ]] && version="$version-$suffix"
        mkdir -p "$BUILD/generated"
        gen_build_info "$version" "$BUILD/generated/build_info_gen.h"
        local sources=("$CORE"/sdk/src/*.cpp "$CORE"/sdk/src/i18n/*.cpp)
        while IFS= read -r f; do sources+=("$f"); done < <(find "$CORE/src" -name '*.cpp' ! -name x11.cpp | sort)
        "$CXX" -std=c++20 -O2 -Wall -Wextra -I"$CORE/src" -I"$CORE/sdk/include" -I"$CORE/sdk/src" -I"$BUILD/generated" \
            "${sources[@]}" -lpthread -o "$BUILD/facet"
    fi
}

compile_plugin() {  # compile_plugin <plugin dir> <output binary>
    local dir="$1" bin="$2" src="$1"
    [[ -d "$dir/src" ]] && src="$dir/src"
    local sources=("$CORE"/sdk/src/*.cpp "$CORE"/sdk/src/i18n/*.cpp)
    while IFS= read -r f; do sources+=("$f"); done < <(find "$src" -name '*.cpp' ! -path '*/build*' ! -path '*/tests/*' ! -path '*/tools/*' | sort)
    "$CXX" -std=c++17 -O2 -I"$src" -I"$CORE/sdk/include" -I"$CORE/sdk/src" "${sources[@]}" -lpthread -o "$bin" >&2
}

# Builds one plugin and prints the path of its executable.
build_plugin() {
    local dir="$1" exec_name="$2" out="$1/build-release"
    if [[ $USE_CMAKE == 1 && -f "$dir/CMakeLists.txt" ]]; then
        cmake -S "$dir" -B "$out" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=Release -DFACET_CORE_DIR="$CORE" \
            -DCMAKE_CXX_COMPILER="$(command -v "$CXX")" >/dev/null
        cmake --build "$out" -j "$JOBS" --target "$exec_name" >&2
        find "$out" -type f -name "$exec_name" -perm -u+x | head -n1
    else
        # Convention for CMake-less builds: every .cpp under src/ (or the plugin
        # directory itself when there is no src/) plus the SDK.
        mkdir -p "$out"
        compile_plugin "$dir" "$out/$exec_name"
        echo "$out/$exec_name"
    fi
}

build_core

declare -A PLUGIN_BIN=()
declare -A PLUGIN_SRC=()

# Bundled plugins (e.g. the default keyboard) ship with the core.
for manifest in "$CORE"/plugins/*/manifest.json; do
    [[ -f "$manifest" ]] || continue
    dir="$(dirname "$manifest")"
    id="$(json_field "$manifest" id)"
    exec_name="$(json_field "$manifest" exec)"
    bin="$BUILD/plugins/$(basename "$dir")/$exec_name"
    if [[ $USE_CMAKE == 0 ]]; then
        say "Building bundled plugin $id"
        mkdir -p "$(dirname "$bin")"
        compile_plugin "$dir" "$bin"
    fi
    [[ -x "$bin" ]] || die "bundled plugin $id was not built ($bin)"
    PLUGIN_BIN[$id]="$bin"
    PLUGIN_SRC[$id]="$dir"
done
for dir in "${PLUGINS[@]}"; do
    [[ -f "$dir/manifest.json" ]] || die "$dir has no manifest.json"
    id="$(json_field "$dir/manifest.json" id)"
    exec_name="$(json_field "$dir/manifest.json" exec)"
    [[ -n "$id" && -n "$exec_name" ]] || die "$dir/manifest.json needs \"id\" and \"exec\""
    say "Building plugin $id"
    if ! bin="$(build_plugin "$dir" "$exec_name")" || [[ ! -x "$bin" ]]; then
        [[ $EXPLICIT_PLUGINS == 1 ]] && die "plugin $id did not build (executable '$exec_name' not found)"
        warn "plugin $id did not build, skipped (see its README for build requirements)"
        continue
    fi
    PLUGIN_BIN[$id]="$bin"
    PLUGIN_SRC[$id]="$dir"
done

# ---------------------------------------------------------------- install

say "Installing into $PREFIX"
if has_systemd && [[ $SERVICE == 1 ]] && systemctl is-active --quiet facet.service; then
    systemctl stop facet.service
fi
install -Dm755 "$BUILD/facet" "$PREFIX/bin/facet"
for id in "${!PLUGIN_BIN[@]}"; do
    exec_name="$(basename "${PLUGIN_BIN[$id]}")"
    install -Dm755 "${PLUGIN_BIN[$id]}" "$PLUGIN_ROOT/$id/$exec_name"
    install -m644 "${PLUGIN_SRC[$id]}/manifest.json" "$PLUGIN_ROOT/$id/manifest.json"
    echo "  plugin $id -> $PLUGIN_ROOT/$id"
done

# Fonts: warn early instead of failing on the panel.
if ! { find /usr/share/fonts /usr/local/share/fonts "$PREFIX/share/facet/fonts" 2>/dev/null || true; } |
        grep -qE '/(OpenSans-Regular|NotoSans-Regular|Ubuntu-R|DejaVuSans|LiberationSans-Regular|regular)\.ttf$'; then
    warn "no supported TrueType font found. Install one: $(pkg_hint fonts-dejavu-core dejavu-sans-fonts ttf-dejavu font-dejavu)"
fi

if [[ $SERVICE == 1 ]]; then
    if ! has_systemd; then
        warn "systemd not found: start $PREFIX/bin/facet from your init system as root"
    else
        say "Installing systemd service"
        sed "s|@FACET_BIN@|$PREFIX/bin/facet|" "$CORE/deploy/facet.service.in" > "$UNIT"
        mkdir -p /etc/facet
        [[ -e /etc/facet/facet.env ]] || install -m644 "$CORE/deploy/facet.env" /etc/facet/facet.env
        systemctl daemon-reload
        systemctl enable facet.service >/dev/null
        if [[ $START == 1 ]]; then
            systemctl restart facet.service
            echo "  started. Logs: journalctl -u facet -f"
        fi
        echo "  settings: /etc/facet/facet.env; data: /var/lib/facet"
        echo "  note: the service takes over tty1 (no local login prompt there)"
    fi
else
    echo "Run it with: sudo $PREFIX/bin/facet   (needs access to /dev/fb0 and /dev/input: root, or groups video+input)"
fi
say "Done: Facet $("$PREFIX/bin/facet" --version 2>/dev/null | head -n1 || echo installed)"
"$PREFIX/bin/facet" --version 2>/dev/null | tail -n +2 | sed 's/^/  /' || true
