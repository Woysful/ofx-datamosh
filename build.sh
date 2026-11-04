#!/bin/sh
# Build release archives of the Datamosh OFX plugin for every platform this
# machine can target. Run ./build.sh --help for usage.
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
OFX_DIR="$ROOT/third_party/openfx"
WORK="$ROOT/build-release"
PLATFORMS="linux windows macos"

usage() {
  cat <<USAGE
Usage: ./build.sh [options] [platform...]

Builds Datamosh.ofx.bundle for each platform and packs it with the README,
LICENSE and install scripts into <output>/ofx-datamosh-<version>-<platform>.zip.
Without platforms, builds every platform that has a toolchain on this machine
and skips the rest.

Platforms:
  linux      native build on Linux (x86_64 or arm64)
  windows    MinGW-w64 cross build, or native in Git Bash / MSYS2 on Windows
  macos      native universal build on a Mac, or osxcross on Linux
  all        same as listing all three

Options:
  -h, --help           show this help and exit
  -l, --list           show which platforms can be built here and exit
  -v, --version VER    version label for archive names
                       (default: git tag on HEAD, else Info.plist version)
  -o, --output DIR     where to put the archives (default: dist)
  -j, --jobs N         parallel compile jobs (default: CPU count)
  -c, --clean          delete previous build files and the output directory first
  -n, --no-zip         only build the bundles, leave them in build-release/<platform>/
  -q, --quiet          hide compiler output (kept in build-release/<platform>.log)
      --debug          debug build instead of release

Environment variables VERSION and JOBS work as defaults for -v and -j.

Examples:
  ./build.sh                     # everything available
  ./build.sh linux windows       # only these two
  ./build.sh -c -v v1.5.0        # clean release build labelled v1.5.0
  gh release create v1.5.0 dist/*.zip
USAGE
}

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

HOST_OS=$(uname -s)
case "$HOST_OS" in
  MINGW*|MSYS*|CYGWIN*) HOST_OS=Windows ;;
esac

# detect_<platform>: print how the platform would be built and return 0,
# or print what is missing and return 1.
detect_linux() {
  if [ "$HOST_OS" = Linux ]; then echo "native ($(uname -m))"
  else echo "only built natively on Linux"; return 1; fi
}
detect_windows() {
  if [ "$HOST_OS" = Windows ]; then
    if have cl.exe || ! have g++; then echo "native (MSVC)"; else echo "native (MinGW)"; fi
  elif have x86_64-w64-mingw32-g++-posix || have x86_64-w64-mingw32-g++; then
    echo "MinGW-w64 cross ($(command -v x86_64-w64-mingw32-g++-posix || command -v x86_64-w64-mingw32-g++))"
  else
    echo "install MinGW-w64 (Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix)"; return 1
  fi
}
detect_macos() {
  if [ "$HOST_OS" = Darwin ]; then echo "native (universal arm64 + x86_64)"
  elif have o64-clang++ && have oa64-clang++; then echo "osxcross (universal arm64 + x86_64)"
  elif have o64-clang++; then echo "osxcross (x86_64 only, SDK has no arm64 compiler)"
  else echo "build on a Mac, or put osxcross' target/bin on PATH"; return 1; fi
}

list_platforms() {
  echo "Host: $HOST_OS $(uname -m)"
  for p in $PLATFORMS; do
    if info=$("detect_$p"); then
      printf '  %-8s yes  %s\n' "$p" "$info"
    else
      printf '  %-8s no   %s\n' "$p" "$info"
    fi
  done
}

VERSION="${VERSION:-}"
JOBS="${JOBS:-}"
DIST="$ROOT/dist"
CLEAN=0; ZIP=1; QUIET=0; BUILD_TYPE=Release
TARGETS=""; EXPLICIT=0

need_arg() { if [ "$#" -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi; }
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)    usage; exit 0 ;;
    -l|--list)    list_platforms; exit 0 ;;
    -v|--version) need_arg "$@"; VERSION="$2"; shift ;;
    --version=*)  VERSION="${1#*=}" ;;
    -o|--output)  need_arg "$@"; DIST="$2"; shift ;;
    --output=*)   DIST="${1#*=}" ;;
    -j|--jobs)    need_arg "$@"; JOBS="$2"; shift ;;
    --jobs=*)     JOBS="${1#*=}" ;;
    -c|--clean)   CLEAN=1 ;;
    -n|--no-zip)  ZIP=0 ;;
    -q|--quiet)   QUIET=1 ;;
    --debug)      BUILD_TYPE=Debug ;;
    all)          TARGETS="$TARGETS $PLATFORMS"; EXPLICIT=1 ;;
    linux|windows|macos) TARGETS="$TARGETS $1"; EXPLICIT=1 ;;
    -*)           die "unknown option: $1 (see --help)" ;;
    *)            die "unknown platform: $1 (use linux, windows, macos or all)" ;;
  esac
  shift
done
[ -n "$TARGETS" ] || TARGETS="$PLATFORMS"
case "$DIST" in /*) ;; *) DIST="$ROOT/$DIST" ;; esac
case "$JOBS" in
  "") JOBS=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4) ;;
  *[!0-9]*) die "--jobs needs a number" ;;
esac

have cmake || die "cmake is required"

if [ -z "$VERSION" ]; then
  VERSION=$(git describe --tags --exact-match 2>/dev/null || true)
  if [ -z "$VERSION" ]; then
    VERSION=$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/v\1/p;}' packaging/Info.plist)
  fi
fi

if [ "$CLEAN" = 1 ]; then
  log "Cleaning $WORK and $DIST"
  rm -rf "$WORK" "$DIST"
fi

if [ ! -f "$OFX_DIR/include/ofxCore.h" ]; then
  log "Fetching OpenFX headers"
  git clone --depth 1 https://github.com/AcademySoftwareFoundation/openfx.git "$OFX_DIR"
fi

# package <build-dir> <platform-name>
package() {
  if [ "$ZIP" = 0 ]; then echo "    $1/Datamosh.ofx.bundle"; return; fi
  stage="$WORK/stage-$2"
  out="$DIST/ofx-datamosh-$VERSION-$2.zip"
  rm -rf "$stage" "$out"
  mkdir -p "$stage" "$DIST"
  cp -R "$1/Datamosh.ofx.bundle" "$stage/"
  cp README.md LICENSE install.sh install.ps1 "$stage/"
  if have zip; then
    (cd "$stage" && zip -qr "$out" .)
  elif have 7z; then
    (cd "$stage" && 7z a -bd "$out" . >/dev/null)
  elif have powershell.exe; then
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$stage")\\*' -DestinationPath '$(cygpath -w "$out")'"
  else
    die "need zip, 7z or PowerShell to make archives (or use --no-zip)"
  fi
  rm -rf "$stage"
  echo "    $out"
}

# compile <build-dir> <cmake args...>
compile() {
  dir="$1"; shift
  rm -rf "$dir"
  if [ "$QUIET" = 1 ]; then
    mkdir -p "$WORK"
    logfile="$dir.log"
    if ! { cmake -S "$ROOT" -B "$dir" -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@" &&
           cmake --build "$dir" --config "$BUILD_TYPE" --parallel "$JOBS"; } >"$logfile" 2>&1; then
      tail -n 20 "$logfile" >&2
      warn "full log: $logfile"
      return 1
    fi
  else
    cmake -S "$ROOT" -B "$dir" -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@" >/dev/null
    cmake --build "$dir" --config "$BUILD_TYPE" --parallel "$JOBS"
  fi
}

build_linux() {
  arch=$(uname -m)
  case "$arch" in aarch64|arm64) name=linux-arm64 ;; *) name="linux-$arch" ;; esac
  compile "$WORK/linux"
  package "$WORK/linux" "$name"
}

build_windows() {
  if [ "$HOST_OS" = Windows ]; then
    if have cl.exe || ! have g++; then
      compile "$WORK/windows" -A x64
    else
      compile "$WORK/windows" -G "MinGW Makefiles"
    fi
  else
    compile "$WORK/windows" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake"
  fi
  package "$WORK/windows" windows-x64
}

build_macos() {
  if [ "$HOST_OS" = Darwin ]; then
    compile "$WORK/macos" "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13
    package "$WORK/macos" macos-universal
    return
  fi
  compile "$WORK/macos-x86_64" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/osxcross.cmake" -DOSX_ARCH=x86_64
  if ! have oa64-clang++; then
    package "$WORK/macos-x86_64" macos-x86_64
    return
  fi
  compile "$WORK/macos-arm64" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/osxcross.cmake" -DOSX_ARCH=arm64
  lipo=$(ls "$(dirname "$(command -v o64-clang++)")"/*-lipo 2>/dev/null | head -n 1)
  [ -n "$lipo" ] || lipo=lipo
  rm -rf "$WORK/macos"
  mkdir -p "$WORK/macos"
  cp -R "$WORK/macos-x86_64/Datamosh.ofx.bundle" "$WORK/macos/"
  "$lipo" -create \
    "$WORK/macos-x86_64/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx" \
    "$WORK/macos-arm64/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx" \
    -output "$WORK/macos/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx"
  package "$WORK/macos" macos-universal
}

built=""; skipped=""; failed=""; done_list=" "
for t in $TARGETS; do
  case "$done_list" in *" $t "*) continue ;; esac
  done_list="$done_list$t "
  if ! info=$("detect_$t"); then
    if [ "$EXPLICIT" = 1 ]; then
      warn "$t: cannot build here: $info"; failed="$failed $t"
    else
      warn "$t: skipped ($info)"; skipped="$skipped $t"
    fi
    continue
  fi
  log "$t: $info, $BUILD_TYPE"
  if ( "build_$t" ); then built="$built $t"; else failed="$failed $t"; warn "$t: build failed"; fi
done

log "Done ($VERSION)"
echo "    built:  ${built:- none}"
[ -z "$skipped" ] || echo "    skipped:$skipped"
[ -z "$failed" ]  || echo "    failed: $failed"
[ -z "$failed" ]
