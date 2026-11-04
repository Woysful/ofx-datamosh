#!/bin/sh
# Install the built bundle into the system-wide OpenFX plugin directory (Linux / macOS).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BUNDLE="$HERE/build/Datamosh.ofx.bundle"
[ -d "$BUNDLE" ] || BUNDLE="$HERE/Datamosh.ofx.bundle"   # release archive layout

case "$(uname -s)" in
  Darwin) DEFAULT_DEST="/Library/OFX/Plugins" ;;
  *)      DEFAULT_DEST="/usr/OFX/Plugins" ;;
esac
DEST="${OFX_INSTALL_DIR:-$DEFAULT_DEST}"

if [ ! -d "$BUNDLE" ]; then
  echo "Bundle not found at $BUNDLE. Build first:" >&2
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

if [ "$(id -u)" -ne 0 ] && [ ! -w "$DEST" ] && [ ! -w "$(dirname "$DEST")" ]; then
  echo "Need write access to $DEST; re-run with sudo." >&2
  exit 1
fi

mkdir -p "$DEST"
rm -rf "$DEST/Datamosh.ofx.bundle" "$DEST/DatamoshTransition.ofx.bundle"
cp -R "$BUNDLE" "$DEST/"
if [ "$(uname -s)" = "Darwin" ]; then
  xattr -dr com.apple.quarantine "$DEST/Datamosh.ofx.bundle" 2>/dev/null || true
fi
echo "Installed to $DEST/Datamosh.ofx.bundle"
echo "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
