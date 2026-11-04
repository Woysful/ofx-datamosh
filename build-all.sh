#!/bin/sh
# Build release archives of the Datamosh OFX plugin for every platform this
# machine can target, and put them in dist/.
#
#   ./build-all.sh                 # all platforms that have a toolchain
#   ./build-all.sh linux windows   # only the listed ones
#
# Platforms:
#   linux    native build (x86_64 or arm64, whatever this Linux machine is)
#   windows  native on Windows shells (MSYS2/Git Bash with cmake + MSVC or MinGW),
#            otherwise cross-compiled with MinGW-w64 (x86_64-w64-mingw32-g++)
#   macos    native on macOS (universal arm64 + x86_64),
#            otherwise cross-compiled with osxcross (o64-clang++ / oa64-clang++ on PATH)
#
# Environment:
#   VERSION  archive version label (default: git tag, else Info.plist version)
#   JOBS     parallel build jobs (default: CPU count)
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
DIST="$ROOT/dist"
OFX_DIR="$ROOT/third_party/openfx"

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

have cmake || { echo "cmake is required" >&2; exit 1; }

if [ -z "${JOBS:-}" ]; then
  JOBS=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)
fi

if [ -z "${VERSION:-}" ]; then
  VERSION=$(git describe --tags --exact-match 2>/dev/null || true)
  if [ -z "$VERSION" ]; then
    VERSION=$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/v\1/p;}' packaging/Info.plist)
  fi
fi

HOST_OS=$(uname -s)
case "$HOST_OS" in
  MINGW*|MSYS*|CYGWIN*) HOST_OS=Windows ;;
esac

if [ ! -f "$OFX_DIR/include/ofxCore.h" ]; then
  log "Fetching OpenFX headers"
  git clone --depth 1 https://github.com/AcademySoftwareFoundation/openfx.git "$OFX_DIR"
fi

# zip_bundle <build-dir> <platform-name>
zip_bundle() {
  stage="$ROOT/build-all/stage-$2"
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
    echo "need zip, 7z or PowerShell to make archives" >&2; return 1
  fi
  echo "    $out"
}

# configure_build <build-dir> <cmake args...>
configure_build() {
  dir="$1"; shift
  rm -rf "$dir"
  cmake -S "$ROOT" -B "$dir" -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
  cmake --build "$dir" --config Release --parallel "$JOBS"
}

build_linux() {
  if [ "$HOST_OS" != Linux ]; then
    warn "linux: skipped (only built natively on Linux)"; return 2
  fi
  arch=$(uname -m)
  case "$arch" in aarch64|arm64) name=linux-arm64 ;; *) name="linux-$arch" ;; esac
  log "Linux ($arch)"
  configure_build build-all/linux
  zip_bundle build-all/linux "$name"
}

build_windows() {
  if [ "$HOST_OS" = Windows ]; then
    log "Windows (native)"
    if have cl.exe || [ -z "$(command -v g++ 2>/dev/null)" ]; then
      configure_build build-all/windows -A x64
    else
      configure_build build-all/windows -G "MinGW Makefiles"
    fi
  elif have x86_64-w64-mingw32-g++-posix || have x86_64-w64-mingw32-g++; then
    log "Windows x64 (MinGW-w64 cross)"
    configure_build build-all/windows -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake"
  else
    warn "windows: skipped (install MinGW-w64, e.g. 'sudo apt install g++-mingw-w64-x86-64-posix')"
    return 2
  fi
  zip_bundle build-all/windows windows-x64
}

build_macos() {
  if [ "$HOST_OS" = Darwin ]; then
    log "macOS universal (native)"
    configure_build build-all/macos "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13
    zip_bundle build-all/macos macos-universal
    return
  fi
  if ! have o64-clang++; then
    warn "macos: skipped (build on a Mac, or put osxcross' target/bin on PATH)"
    return 2
  fi
  log "macOS (osxcross)"
  configure_build build-all/macos-x86_64 -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/osxcross.cmake" -DOSX_ARCH=x86_64
  if ! have oa64-clang++; then
    warn "macos: osxcross SDK has no arm64 compiler; archive is x86_64 only"
    zip_bundle build-all/macos-x86_64 macos-x86_64
    return
  fi
  configure_build build-all/macos-arm64 -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/osxcross.cmake" -DOSX_ARCH=arm64
  lipo=$(ls "$(dirname "$(command -v o64-clang++)")"/*-lipo 2>/dev/null | head -n 1)
  [ -n "$lipo" ] || lipo=lipo
  rm -rf build-all/macos
  mkdir -p build-all/macos
  cp -R build-all/macos-x86_64/Datamosh.ofx.bundle build-all/macos/
  "$lipo" -create \
    build-all/macos-x86_64/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx \
    build-all/macos-arm64/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx \
    -output build-all/macos/Datamosh.ofx.bundle/Contents/MacOS/Datamosh.ofx
  zip_bundle build-all/macos macos-universal
}

TARGETS="${*:-linux windows macos}"
built=""; skipped=""; failed=""
for t in $TARGETS; do
  case "$t" in
    linux|windows|macos) ;;
    *) echo "unknown platform: $t (use linux, windows, macos)" >&2; exit 1 ;;
  esac
  rc=0
  ( "build_$t" ) || rc=$?
  case $rc in
    0) built="$built $t" ;;
    2) skipped="$skipped $t" ;;
    *) failed="$failed $t"; warn "$t: build failed" ;;
  esac
done

log "Done ($VERSION)"
echo "    built:  ${built:- none}"
[ -z "$skipped" ] || echo "    skipped:$skipped"
[ -z "$failed" ]  || echo "    failed: $failed"
[ -z "$failed" ]
