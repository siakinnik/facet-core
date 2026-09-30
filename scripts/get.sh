#!/usr/bin/env bash
# Installs prebuilt Facet releases from GitHub: the core, or a plugin.
# No compiler needed.
#
#   curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash
#   ... | sudo bash -s -- --version v0.0.1-alpha     install a specific core release
#   ... | sudo bash -s -- --no-start                 install and enable, start on next boot
#   ... | sudo bash -s -- --uninstall                remove the core (keeps settings, data, plugins)
#   ... | sudo bash -s -- --plugin OWNER/REPO        install or update a plugin from its releases
#   ... | sudo bash -s -- --remove-plugin ID         remove an installed plugin
#   sudo scripts/get.sh --file archive.tar.gz [--plugin OWNER/REPO]   install a downloaded archive
set -euo pipefail

REPO="${FACET_REPO:-siakinnik/facet-core}"
PREFIX=/usr/local
TAG=""
FILE=""
PLUGIN=""
REMOVE_PLUGIN=""
SERVICE=1
START=1
UNINSTALL=0

usage() {  # inline: when piped from curl, $0 is not this file
    echo "Usage: get.sh [--version vX.Y.Z] [--file archive.tar.gz] [--prefix DIR]"
    echo "              [--no-service] [--no-start] [--uninstall]"
    echo "       get.sh --plugin OWNER/REPO [--version vX.Y.Z] [--file archive.tar.gz]"
    echo "       get.sh --remove-plugin ID"
    echo "Installs prebuilt Facet releases from GitHub (default: the newest one)."
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) TAG="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --plugin) PLUGIN="$2"; shift 2 ;;
        --remove-plugin) REMOVE_PLUGIN="$2"; shift 2 ;;
        --no-service) SERVICE=0; shift ;;
        --no-start) START=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

UNIT=/etc/systemd/system/facet.service
PLUGIN_ROOT="$PREFIX/lib/facet/plugins"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
has_systemd() { [[ -d /run/systemd/system ]] && have systemctl; }
fetch() {  # fetch <url> <out-file>
    if have curl; then curl -fsSL --retry 3 -o "$2" "$1"
    elif have wget; then wget -q -O "$2" "$1"
    else die "need curl or wget"; fi
}
valid_id() { [[ "$1" =~ ^[a-z0-9._-]{1,64}$ ]]; }
restart_facet() {  # plugins are discovered at start
    if has_systemd && systemctl is-active --quiet facet.service; then
        say "Restarting Facet"
        systemctl restart facet.service
    else
        echo "  restart Facet to load the change"
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- remove plugin
if [[ -n "$REMOVE_PLUGIN" ]]; then
    valid_id "$REMOVE_PLUGIN" || die "invalid plugin id: $REMOVE_PLUGIN"
    [[ -d "$PLUGIN_ROOT/$REMOVE_PLUGIN" ]] || die "plugin $REMOVE_PLUGIN is not installed in $PLUGIN_ROOT"
    [[ -w "$PLUGIN_ROOT" ]] || die "run as root (sudo)"
    rm -rf "${PLUGIN_ROOT:?}/$REMOVE_PLUGIN"
    say "Removed plugin $REMOVE_PLUGIN (its settings stay in /var/lib/facet/data/$REMOVE_PLUGIN)"
    restart_facet
    exit 0
fi

if [[ -n "$PLUGIN" ]]; then
    [[ "$PLUGIN" =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || die "--plugin expects OWNER/REPO, got: $PLUGIN"
    mkdir -p "$PLUGIN_ROOT" 2>/dev/null || true
    [[ -w "$PLUGIN_ROOT" ]] || die "run as root (sudo)"
elif [[ $EUID -ne 0 && ( $SERVICE == 1 || ! -w "$PREFIX" ) ]]; then
    die "run as root (sudo), or use --prefix ~/.local --no-service"
fi

# ---------------------------------------------------------------- uninstall
if [[ $UNINSTALL == 1 ]]; then
    if has_systemd && [[ -e $UNIT ]]; then
        say "Stopping and removing the service"
        systemctl disable --now facet.service || true
        rm -f "$UNIT"
        systemctl daemon-reload
    fi
    rm -f "$PREFIX/bin/facet"
    rm -rf "$PREFIX/share/facet"
    say "Removed. Kept /etc/facet, /var/lib/facet and plugins in $PREFIX/lib/facet."
    exit 0
fi

# ---------------------------------------------------------------- download
[[ "$(uname -s)" == Linux ]] || die "Facet runs on Linux only"
case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    armv7l|armv8l|armhf) ARCH=armv7 ;;
    *) die "no prebuilt binary for $(uname -m); build from source" ;;
esac

# Release archives: <prefix>-<version>-linux-<arch>.tar.gz (core: "facet", plugins: repo name).
download() {  # download <owner/repo> <archive prefix> -> path of the archive
    local repo="$1" tag="$TAG" name base
    if [[ -z "$tag" ]]; then
        # Newest release including pre-releases (/releases/latest skips those).
        fetch "https://api.github.com/repos/$repo/releases?per_page=1" "$WORK/releases.json" ||
            die "cannot reach GitHub releases of $repo"
        tag="$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$WORK/releases.json" | head -n1)"
        [[ -n "$tag" ]] || die "no releases found in $repo"
    fi
    name="$2-${tag#v}-linux-$ARCH.tar.gz"
    base="https://github.com/$repo/releases/download/$tag"
    say "Downloading $repo $tag for $ARCH" >&2
    fetch "$base/$name" "$WORK/$name" || die "cannot download $base/$name"
    if fetch "$base/SHA256SUMS" "$WORK/SHA256SUMS" 2>/dev/null && have sha256sum; then
        (cd "$WORK" && grep " $name\$" SHA256SUMS | sha256sum -c --quiet -) || die "checksum mismatch for $name"
    else
        warn "could not verify the checksum"
    fi
    echo "$WORK/$name"
}

# ---------------------------------------------------------------- plugin
if [[ -n "$PLUGIN" ]]; then
    [[ -n "$FILE" ]] || FILE="$(download "$PLUGIN" "${PLUGIN##*/}")"  # plugin archives: <repo>-...
    mkdir -p "$WORK/pkg"
    tar -C "$WORK/pkg" -xzf "$FILE"
    manifest="$(find "$WORK/pkg" -mindepth 2 -maxdepth 2 -name manifest.json | head -n1)"
    [[ -n "$manifest" ]] || die "archive has no <plugin>/manifest.json"
    id="$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -n1)"
    version="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -n1)"
    valid_id "$id" || die "manifest.json has an invalid id: $id"
    [[ -x "$PREFIX/bin/facet" ]] || warn "Facet core not found in $PREFIX/bin; install it first (run this script without --plugin)"

    # Replace atomically-ish: stage next to the target, then swap.
    say "Installing plugin $id $version into $PLUGIN_ROOT/$id"
    rm -rf "${PLUGIN_ROOT:?}/.$id.new"
    cp -r "$(dirname "$manifest")" "$PLUGIN_ROOT/.$id.new"
    chmod -R u=rwX,go=rX "$PLUGIN_ROOT/.$id.new"
    rm -rf "${PLUGIN_ROOT:?}/$id"
    mv "$PLUGIN_ROOT/.$id.new" "$PLUGIN_ROOT/$id"
    restart_facet
    say "Done"
    exit 0
fi

# ---------------------------------------------------------------- core
[[ -n "$FILE" ]] || FILE="$(download "$REPO" facet)"  # core archives: facet-<version>-...
tar -C "$WORK" -xzf "$FILE"
PKG="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'facet-*' | head -n1)"
[[ -n "$PKG" && -x "$PKG/bin/facet" ]] || die "archive does not contain bin/facet"

if has_systemd && [[ $SERVICE == 1 ]] && systemctl is-active --quiet facet.service; then
    systemctl stop facet.service
fi
say "Installing into $PREFIX"
install -Dm755 "$PKG/bin/facet" "$PREFIX/bin/facet"
install -Dm644 "$PKG/share/facet/facet.env" "$PREFIX/share/facet/facet.env"
install -Dm644 "$PKG/share/facet/facet.service.in" "$PREFIX/share/facet/facet.service.in"
mkdir -p "$PLUGIN_ROOT"

if ! { find /usr/share/fonts /usr/local/share/fonts 2>/dev/null || true; } |
        grep -qE '/(OpenSans-Regular|NotoSans-Regular|Ubuntu-R|DejaVuSans|LiberationSans-Regular)\.ttf$'; then
    warn "no supported font found; install one, e.g. sudo apt install fonts-dejavu-core"
fi

if [[ $SERVICE == 1 ]]; then
    if ! has_systemd; then
        warn "systemd not found: start $PREFIX/bin/facet as root from your init system"
    else
        say "Installing systemd service"
        sed "s|@FACET_BIN@|$PREFIX/bin/facet|" "$PKG/share/facet/facet.service.in" > "$UNIT"
        mkdir -p /etc/facet
        [[ -e /etc/facet/facet.env ]] || install -m644 "$PKG/share/facet/facet.env" /etc/facet/facet.env
        systemctl daemon-reload
        systemctl enable facet.service >/dev/null
        [[ $START == 1 ]] && systemctl restart facet.service
        echo "  logs: journalctl -u facet -f; settings: /etc/facet/facet.env"
        echo "  note: the service takes over tty1 (no local login prompt there)"
    fi
fi
say "Done"
"$PREFIX/bin/facet" --version | sed 's/^/  /'
