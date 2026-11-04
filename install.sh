#!/bin/sh
# Install Datamosh.ofx.bundle into the system-wide OpenFX plugin directory
# (Linux / macOS), from a local build or from the latest GitHub release.
# Run ./install.sh --help for usage. One-line install:
#   curl -fsSL https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.sh | sh
set -eu

REPO="${OFX_DATAMOSH_REPO:-Woysful/ofx-datamosh}"
API_URL="${OFX_DATAMOSH_API:-https://api.github.com/repos/$REPO/releases/latest}"
BUNDLE_NAME=Datamosh.ofx.bundle

# Run from a file (./install.sh, sh install.sh) or piped (curl ... | sh)?
case "$0" in
  *install.sh) HERE="$(cd "$(dirname "$0")" && pwd)" ;;
  *)           HERE="" ;;
esac

usage() {
  cat <<USAGE
Usage: ./install.sh [options] [bundle-or-zip]
       curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh

Installs $BUNDLE_NAME into the OpenFX plugin directory and removes older
versions of the plugin. The plugin comes either from a local build or from the
latest release on https://github.com/$REPO/releases.

In a terminal you are asked which one to use. Without a terminal the local
build is used if there is one, otherwise the latest release is downloaded.

Local builds are searched for (newest build for this OS wins) in:
  ./Datamosh.ofx.bundle            (unpacked release archive)
  build-release/<platform>/        (./build.sh)
  build/                           (plain cmake build)
  dist/ofx-datamosh-*.zip          (./build.sh archives)

Downloads are checked against the SHA-256 published with the release.

Options:
  -h, --help        show this help and exit
  -l, --local       use the local build, do not ask
  -r, --download    download the latest release, do not ask
  -d, --dest DIR    install into DIR (default: $DEFAULT_DEST,
                    or \$OFX_INSTALL_DIR if set)

Writing to the system directory needs administrator rights; the script runs
the copy step through sudo when needed. With piped install pass options as:
  curl -fsSL .../install.sh | sh -s -- --download
USAGE
}

say()  { printf '%s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

case "$(uname -s)" in
  Darwin)
    OS=macos; ARCH_DIR=MacOS; DEFAULT_DEST="/Library/OFX/Plugins"
    case "$(uname -m)" in
      arm64) ASSETS="macos-universal macos-arm64" ;;
      *)     ASSETS="macos-universal macos-x86_64" ;;
    esac ;;
  Linux)
    OS=linux; DEFAULT_DEST="/usr/OFX/Plugins"
    case "$(uname -m)" in
      x86_64|amd64)  ARCH_DIR=Linux-x86-64; ASSETS=linux-x86_64 ;;
      aarch64|arm64) ARCH_DIR=Linux-arm64;  ASSETS=linux-arm64 ;;
      *)             ARCH_DIR="Linux-$(uname -m)"; ASSETS="linux-$(uname -m)" ;;
    esac ;;
  MINGW*|MSYS*|CYGWIN*) die "on Windows run install.ps1 from an elevated PowerShell" ;;
  *) die "unsupported OS: $(uname -s)" ;;
esac
DEST="${OFX_INSTALL_DIR:-$DEFAULT_DEST}"

MODE=""; SOURCE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)     usage; exit 0 ;;
    -l|--local)    MODE=local ;;
    -r|--download) MODE=download ;;
    -d|--dest)     if [ "$#" -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi; DEST="$2"; shift ;;
    --dest=*)      DEST="${1#*=}" ;;
    -*)            die "unknown option: $1 (see --help)" ;;
    *)             [ -z "$SOURCE" ] || die "only one bundle or archive can be given"; SOURCE="$1" ;;
  esac
  shift
done
[ -n "$DEST" ] || die "empty install directory"
if [ -n "$SOURCE" ]; then
  [ "$MODE" != download ] || die "--download cannot be combined with a bundle or archive"
  MODE=local
fi

# Where to read answers from: the terminal on stdin, or /dev/tty when the
# script itself arrives on stdin (curl ... | sh). Empty means no terminal.
TTY=""
if [ -t 0 ]; then
  TTY=/dev/stdin
elif [ -t 1 ] && (: </dev/tty) 2>/dev/null; then
  TTY=/dev/tty
fi
ask() {
  printf '%s' "$1" >&2
  ANSWER=""
  read -r ANSWER <"$TTY" || ANSWER=""
}

TMP=""
cleanup() { if [ -n "$TMP" ]; then rm -rf "$TMP"; fi; }
trap cleanup EXIT
trap 'exit 130' INT TERM
tmpdir() { if [ -z "$TMP" ]; then TMP=$(mktemp -d); fi; }

binary_of() { echo "$1/Contents/$ARCH_DIR/Datamosh.ofx"; }
usable() { [ -f "$(binary_of "$1")" ]; }
newer() { [ -n "$(find "$1" -prune -newer "$2")" ]; }
mtime() { date -r "$1" '+%Y-%m-%d %H:%M' 2>/dev/null || echo "unknown date"; }

# Sets BUNDLE to the bundle inside zip $1.
unpack_zip() {
  have unzip || die "unzip is needed to install from $1"
  tmpdir
  rm -rf "$TMP/unpacked"
  unzip -q "$1" -d "$TMP/unpacked"
  [ -d "$TMP/unpacked/$BUNDLE_NAME" ] || die "$1 does not contain $BUNDLE_NAME"
  BUNDLE="$TMP/unpacked/$BUNDLE_NAME"
  usable "$BUNDLE" || die "$1 has no $ARCH_DIR binary, it was not built for this OS"
}

# Sets LOCAL to the newest local bundle or archive for this OS, or "".
find_local() {
  LOCAL=""
  [ -n "$HERE" ] || return 0
  for b in "$HERE/$BUNDLE_NAME" \
           "$HERE/build-release/$OS/$BUNDLE_NAME" \
           "$HERE/build-release/$OS-x86_64/$BUNDLE_NAME" \
           "$HERE/build/$BUNDLE_NAME"; do
    if usable "$b"; then
      if [ -z "$LOCAL" ] || newer "$(binary_of "$b")" "$(binary_of "$LOCAL")"; then LOCAL="$b"; fi
    fi
  done
  [ -z "$LOCAL" ] || return 0
  for a in $ASSETS; do
    for z in "$HERE"/dist/ofx-datamosh-*-"$a".zip; do
      [ -f "$z" ] || continue
      if [ -z "$LOCAL" ] || newer "$z" "$LOCAL"; then LOCAL="$z"; fi
    done
  done
}

fetch() {  # fetch URL [OUT]; without OUT writes to stdout
  if have curl; then
    if [ $# -ge 2 ]; then curl -fsSL --retry 2 -o "$2" "$1"; else curl -fsSL --retry 2 "$1"; fi
  elif have wget; then
    if [ $# -ge 2 ]; then wget -q -O "$2" "$1"; else wget -q -O - "$1"; fi
  else
    die "curl or wget is needed to download the release"
  fi
}

# Sets REL_STATE (ok / noasset / offline), REL_TAG, REL_URL, REL_DIGEST, REL_SUMS.
release_info() {
  REL_STATE=offline; REL_TAG=""; REL_URL=""; REL_DIGEST=""; REL_SUMS=""
  tmpdir
  fetch "$API_URL" "$TMP/release.json" 2>/dev/null || return 0
  awk -v assets="$ASSETS" '
    function val(s) { sub(/^[^:]*: *"/, "", s); sub(/".*$/, "", s); return s }
    BEGIN { n = split(assets, want, " ") }
    /"tag_name":/ && tag == "" { tag = val($0); print "tag " tag }
    /"digest": *"/ { d = val($0) }
    /"browser_download_url":/ {
      u = val($0)
      for (i = 1; i <= n; i++) {
        suf = "-" want[i] ".zip"
        if (length(u) >= length(suf) && substr(u, length(u) - length(suf) + 1) == suf && !(i in found)) {
          found[i] = u " " (d == "" ? "-" : d)
        }
      }
      if (u ~ /\/SHA256SUMS$/) print "sums " u
      d = ""
    }
    END { for (i = 1; i <= n; i++) if (i in found) { print "zip " found[i]; exit } }
  ' "$TMP/release.json" > "$TMP/release.txt"
  REL_TAG=$(sed -n 's/^tag //p' "$TMP/release.txt")
  [ -n "$REL_TAG" ] || return 0
  REL_STATE=noasset
  REL_SUMS=$(sed -n 's/^sums //p' "$TMP/release.txt")
  line=$(sed -n 's/^zip //p' "$TMP/release.txt")
  [ -n "$line" ] || return 0
  REL_URL=${line% *}
  REL_DIGEST=${line##* }
  [ "$REL_DIGEST" != "-" ] || REL_DIGEST=""
  REL_STATE=ok
}

sha256_of() {
  if have sha256sum; then sha256sum "$1" | awk '{print $1}'
  elif have shasum; then shasum -a 256 "$1" | awk '{print $1}'
  elif have openssl; then openssl dgst -sha256 -r "$1" | awk '{print $1}'
  else echo ""; fi
}

# Downloads the release archive for this OS and sets BUNDLE.
download_release() {
  [ -n "${REL_STATE:-}" ] || release_info
  case "$REL_STATE" in
    offline) die "could not get release information from $API_URL" ;;
    noasset) die "release $REL_TAG has no archive for this OS ($ASSETS); build from source: ./build.sh --install" ;;
  esac
  name=${REL_URL##*/}
  say "Downloading $name ($REL_TAG)"
  tmpdir
  fetch "$REL_URL" "$TMP/$name" || die "download failed: $REL_URL"

  expected=""
  case "$REL_DIGEST" in sha256:*) expected=${REL_DIGEST#sha256:} ;; esac
  if [ -z "$expected" ] && [ -n "$REL_SUMS" ]; then
    expected=$(fetch "$REL_SUMS" 2>/dev/null | awk -v n="$name" '$2 == n || $2 == "*" n {print $1; exit}' || true)
  fi
  if [ -z "$expected" ]; then
    say "warning: the release publishes no SHA-256 for $name, skipping verification"
  else
    actual=$(sha256_of "$TMP/$name")
    [ -n "$actual" ] || die "no sha256sum, shasum or openssl to verify the download"
    expected=$(printf '%s' "$expected" | tr 'A-F' 'a-f')
    [ "$actual" = "$expected" ] || die "SHA-256 mismatch for $name (expected $expected, got $actual); not installing"
    say "SHA-256 OK"
  fi
  unpack_zip "$TMP/$name"
}

use_local() {  # use_local PATH: sets BUNDLE from a bundle dir or zip
  case "$1" in
    *.zip) [ -f "$1" ] || die "no such file: $1"; unpack_zip "$1" ;;
    *)     BUNDLE="$1"
           [ -d "$BUNDLE/Contents" ] || BUNDLE="$1/$BUNDLE_NAME"
           usable "$BUNDLE" || die "$1 has no $ARCH_DIR binary, it was not built for this OS" ;;
  esac
}

no_local_msg() {
  say "No local build for this OS ($ARCH_DIR) was found. Build one with ./build.sh,"
  say "or build and install in one go: ./build.sh --install"
}

BUNDLE=""
case "$MODE" in
  local)
    if [ -n "$SOURCE" ]; then use_local "$SOURCE"; else
      find_local
      if [ -z "$LOCAL" ]; then no_local_msg; exit 1; fi
      use_local "$LOCAL"
    fi ;;
  download)
    download_release ;;
  *)
    find_local
    if [ -z "$TTY" ]; then
      if [ -n "$LOCAL" ]; then use_local "$LOCAL"; else download_release; fi
    else
      release_info
      say "Datamosh OFX installer ($OS, $ARCH_DIR)"
      choices=""
      if [ -n "$LOCAL" ]; then
        say "  1) local build:    ${LOCAL#"$HERE"/} ($(mtime "$LOCAL"))"
        choices="1"
      else
        say "     (no local build for this OS)"
      fi
      case "$REL_STATE" in
        ok)      say "  2) latest release: $REL_TAG from github.com/$REPO"; choices="$choices 2" ;;
        noasset) say "     (latest release $REL_TAG has no archive for this OS)" ;;
        offline) say "     (could not reach GitHub for the latest release)" ;;
      esac
      case "$choices" in
        "")  no_local_msg; exit 1 ;;
        "1"|" 2")
          c=${choices# }
          if [ "$c" = 1 ]; then ask "Install the local build? [Y/n] "
          else ask "Download and install $REL_TAG? [Y/n] "; fi
          case "$ANSWER" in ""|y|Y|yes|Yes) ;; *) say "Cancelled."; exit 1 ;; esac ;;
        *)
          while :; do
            ask "Choose 1 or 2, q to quit [1]: "
            case "$ANSWER" in
              ""|1) c=1; break ;;
              2)    c=2; break ;;
              q|Q)  say "Cancelled."; exit 1 ;;
            esac
          done ;;
      esac
      if [ "$c" = 1 ]; then use_local "$LOCAL"; else download_release; fi
    fi ;;
esac

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  probe="$DEST"
  while [ ! -e "$probe" ]; do probe=$(dirname "$probe"); done
  if [ ! -w "$probe" ]; then
    have sudo || die "need write access to $DEST; re-run as root"
    say "$DEST needs administrator rights, using sudo"
    SUDO=sudo
  fi
fi

say "Installing $BUNDLE"
$SUDO mkdir -p "$DEST"
$SUDO rm -rf "${DEST:?}/$BUNDLE_NAME" "$DEST/DatamoshTransition.ofx.bundle"
$SUDO cp -R "$BUNDLE" "$DEST/"
if [ "$OS" = macos ]; then
  $SUDO xattr -dr com.apple.quarantine "$DEST/$BUNDLE_NAME" 2>/dev/null || true
fi
say "Installed to $DEST/$BUNDLE_NAME"
say "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
