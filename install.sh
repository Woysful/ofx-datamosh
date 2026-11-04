#!/bin/sh
# Install Datamosh.ofx.bundle into the system-wide OpenFX plugin directory
# (Linux / macOS). Run ./install.sh --help for usage.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BUNDLE_NAME=Datamosh.ofx.bundle

usage() {
  cat <<USAGE
Usage: ./install.sh [options] [bundle-or-zip]

Copies $BUNDLE_NAME into the OpenFX plugin directory and removes older
versions of the plugin. Without an argument it uses the newest build for this
OS found in:
  ./Datamosh.ofx.bundle            (unpacked release archive)
  build-release/<platform>/        (./build.sh)
  build/                           (plain cmake build)
  dist/ofx-datamosh-*.zip          (./build.sh archives, unpacked on the fly)

Options:
  -h, --help        show this help and exit
  -d, --dest DIR    install into DIR (default: $DEFAULT_DEST,
                    or \$OFX_INSTALL_DIR if set)

Writing to the system directory needs root: sudo ./install.sh
USAGE
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) OS=macos; ARCH_DIR=MacOS; DEFAULT_DEST="/Library/OFX/Plugins" ;;
  Linux)
    OS=linux; DEFAULT_DEST="/usr/OFX/Plugins"
    case "$(uname -m)" in
      x86_64|amd64)  ARCH_DIR=Linux-x86-64 ;;
      aarch64|arm64) ARCH_DIR=Linux-arm64 ;;
      *)             ARCH_DIR="Linux-$(uname -m)" ;;
    esac ;;
  MINGW*|MSYS*|CYGWIN*) die "on Windows run install.ps1 from an elevated PowerShell" ;;
  *) die "unsupported OS: $(uname -s)" ;;
esac
DEST="${OFX_INSTALL_DIR:-$DEFAULT_DEST}"

SOURCE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -d|--dest) [ "$#" -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; DEST="$2"; shift ;;
    --dest=*)  DEST="${1#*=}" ;;
    -*)        die "unknown option: $1 (see --help)" ;;
    *)         [ -z "$SOURCE" ] || die "only one bundle or archive can be given"; SOURCE="$1" ;;
  esac
  shift
done

TMP=""
cleanup() { if [ -n "$TMP" ]; then rm -rf "$TMP"; fi; }
trap cleanup EXIT INT TERM

# A bundle is usable if it carries a binary for this OS.
binary_of() { echo "$1/Contents/$ARCH_DIR/Datamosh.ofx"; }
usable() { [ -f "$(binary_of "$1")" ]; }
newer() { [ -n "$(find "$1" -prune -newer "$2")" ]; }

# Sets BUNDLE; not used via $(...) so the EXIT trap still sees TMP.
unpack_zip() {
  command -v unzip >/dev/null 2>&1 || die "unzip is needed to install from $1"
  TMP=$(mktemp -d)
  unzip -q "$1" -d "$TMP"
  [ -d "$TMP/$BUNDLE_NAME" ] || die "$1 does not contain $BUNDLE_NAME"
  BUNDLE="$TMP/$BUNDLE_NAME"
}

BUNDLE=""
if [ -n "$SOURCE" ]; then
  case "$SOURCE" in
    *.zip) [ -f "$SOURCE" ] || die "no such file: $SOURCE"; unpack_zip "$SOURCE" ;;
    *)     BUNDLE="$SOURCE"
           [ -d "$BUNDLE/Contents" ] || BUNDLE="$SOURCE/$BUNDLE_NAME" ;;
  esac
  usable "$BUNDLE" || die "$SOURCE has no $ARCH_DIR binary, it was not built for this OS"
else
  for b in "$HERE/$BUNDLE_NAME" \
           "$HERE/build-release/$OS/$BUNDLE_NAME" \
           "$HERE/build-release/$OS-x86_64/$BUNDLE_NAME" \
           "$HERE/build/$BUNDLE_NAME"; do
    if usable "$b"; then
      if [ -z "$BUNDLE" ] || newer "$(binary_of "$b")" "$(binary_of "$BUNDLE")"; then BUNDLE="$b"; fi
    fi
  done
  if [ -z "$BUNDLE" ]; then
    zip=""
    for z in "$HERE"/dist/ofx-datamosh-*-"$OS"-*.zip; do
      [ -f "$z" ] || continue
      if [ -z "$zip" ] || newer "$z" "$zip"; then zip="$z"; fi
    done
    if [ -n "$zip" ]; then
      unpack_zip "$zip"
      usable "$BUNDLE" || die "$zip has no $ARCH_DIR binary"
    fi
  fi
fi

if [ -z "$BUNDLE" ]; then
  cat >&2 <<MSG
error: no $BUNDLE_NAME built for this OS ($ARCH_DIR) was found.
Build it first, then run this script again:
  ./build.sh $OS              # or build and install in one go: ./build.sh --install
MSG
  exit 1
fi

if [ "$(id -u)" -ne 0 ] && [ ! -w "$DEST" ] && [ ! -w "$(dirname "$DEST")" ]; then
  die "need write access to $DEST; re-run with sudo"
fi

echo "Installing $BUNDLE"
mkdir -p "$DEST"
rm -rf "${DEST:?}/$BUNDLE_NAME" "$DEST/DatamoshTransition.ofx.bundle"
cp -R "$BUNDLE" "$DEST/"
if [ "$OS" = macos ]; then
  xattr -dr com.apple.quarantine "$DEST/$BUNDLE_NAME" 2>/dev/null || true
fi
echo "Installed to $DEST/$BUNDLE_NAME"
echo "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
