#!/usr/bin/env bash
# Installs a prebuilt Facet release from GitHub. No compiler needed.
#
#   curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash
#   ... | sudo bash -s -- --version v0.0.1-alpha     install a specific release
#   ... | sudo bash -s -- --no-start                 install and enable, start on next boot
#   ... | sudo bash -s -- --uninstall                remove (keeps settings and data)
#   sudo scripts/get.sh --file facet-...tar.gz        install a downloaded archive
set -euo pipefail

REPO="${FACET_REPO:-siakinnik/facet-core}"
PREFIX=/usr/local
TAG=""
FILE=""
SERVICE=1
START=1
UNINSTALL=0

usage() {  # inline: when piped from curl, $0 is not this file
    echo "Usage: get.sh [--version vX.Y.Z] [--file archive.tar.gz] [--prefix DIR]"
    echo "              [--no-service] [--no-start] [--uninstall]"
    echo "Installs a prebuilt Facet release from GitHub (default: the newest one)."
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) TAG="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --no-service) SERVICE=0; shift ;;
        --no-start) START=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

UNIT=/etc/systemd/system/facet.service
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

if [[ $EUID -ne 0 && ( $SERVICE == 1 || ! -w "$PREFIX" ) ]]; then
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
    *) die "no prebuilt binary for $(uname -m); build from source with scripts/install.sh" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [[ -z "$FILE" ]]; then
    if [[ -z "$TAG" ]]; then
        # Newest release including pre-releases (/releases/latest skips those).
        fetch "https://api.github.com/repos/$REPO/releases?per_page=1" "$WORK/releases.json"
        TAG="$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$WORK/releases.json" | head -n1)"
        [[ -n "$TAG" ]] || die "no releases found in $REPO"
    fi
    NAME="facet-${TAG#v}-linux-$ARCH.tar.gz"
    BASE="https://github.com/$REPO/releases/download/$TAG"
    say "Downloading Facet $TAG for $ARCH"
    fetch "$BASE/$NAME" "$WORK/$NAME" || die "cannot download $BASE/$NAME"
    if fetch "$BASE/SHA256SUMS" "$WORK/SHA256SUMS" 2>/dev/null && have sha256sum; then
        (cd "$WORK" && grep " $NAME\$" SHA256SUMS | sha256sum -c --quiet -) || die "checksum mismatch for $NAME"
    else
        warn "could not verify the checksum"
    fi
    FILE="$WORK/$NAME"
fi

tar -C "$WORK" -xzf "$FILE"
PKG="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'facet-*' | head -n1)"
[[ -x "$PKG/bin/facet" ]] || die "archive does not contain bin/facet"

# ---------------------------------------------------------------- install
if has_systemd && [[ $SERVICE == 1 ]] && systemctl is-active --quiet facet.service; then
    systemctl stop facet.service
fi
say "Installing into $PREFIX"
install -Dm755 "$PKG/bin/facet" "$PREFIX/bin/facet"
install -Dm644 "$PKG/share/facet/facet.env" "$PREFIX/share/facet/facet.env"
install -Dm644 "$PKG/share/facet/facet.service.in" "$PREFIX/share/facet/facet.service.in"
mkdir -p "$PREFIX/lib/facet/plugins"

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
