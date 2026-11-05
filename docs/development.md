# Development

[Back to README](../README.md)

## Build from source

Requirements: a C++17 compiler, CMake 3.16+, git. No other dependencies;
the OpenFX headers are fetched into `third_party/`.

**Linux / macOS**

```sh
git clone https://github.com/Woysful/ofx-datamosh.git
cd ofx-datamosh
git clone --depth 1 https://github.com/AcademySoftwareFoundation/openfx.git third_party/openfx
./build.sh --install            # build for this OS and install (asks for sudo)
```

Or with plain CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./install.sh --local
```

On macOS add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` to the configure
step for a universal binary.

**Windows** (Visual Studio 2019/2022 with the C++ workload, or the Build Tools)

```powershell
git clone https://github.com/Woysful/ofx-datamosh.git
cd ofx-datamosh
git clone --depth 1 https://github.com/AcademySoftwareFoundation/openfx.git third_party/openfx
cmake -S . -B build -A x64
cmake --build build --config Release
# as Administrator:
.\install.ps1
```

The bundle lands in `build/Datamosh.ofx.bundle/`. On Linux the C++ runtime
is linked statically so the plugin does not clash with the libraries Resolve
ships.

**Release archives for every platform**

`build.sh` builds every platform the machine has a toolchain for and writes
`dist/ofx-datamosh-<version>-<platform>.zip` (bundle, README, LICENSE,
install scripts). The version is the git tag on the current commit, or the
version from `packaging/Info.plist`.

```sh
./build.sh --list              # which platforms can be built on this machine
./build.sh                     # all of them; the rest are skipped
./build.sh linux windows       # only these (fails if one cannot be built)
./build.sh -c -q -v v1.5.0     # clean, quiet build labelled v1.5.0
./build.sh -i linux            # build for Linux and install it
./build.sh --help              # all options
```

| Option | Meaning |
|---|---|
| `-h`, `--help` | show usage |
| `-l`, `--list` | show which platforms can be built here and how |
| `-v`, `--version VER` | version label in archive names |
| `-o`, `--output DIR` | output directory (default `dist`) |
| `-j`, `--jobs N` | parallel compile jobs |
| `-c`, `--clean` | delete `build-release/` and the output directory first |
| `-n`, `--no-zip` | only build bundles, left in `build-release/<platform>/` |
| `-q`, `--quiet` | hide compiler output, keep it in `build-release/<platform>.log` |
| `--debug` | debug build |
| `-i`, `--install` | install the build for this OS afterwards (uses sudo if needed) |
| `--install-dir DIR` | install into DIR instead of the system plugin directory (implies `--install`) |

| Platform | How it is built | What you need |
|---|---|---|
| linux | native, on Linux (x86_64 or arm64) | g++ or clang |
| windows | MinGW-w64 cross compiler on Linux/macOS, or natively in Git Bash / MSYS2 on Windows | `g++-mingw-w64-x86-64-posix` (Debian/Ubuntu), `mingw64-gcc-c++` (Fedora), `mingw-w64-gcc` (Arch), `mingw-w64` (Homebrew) |
| macos | natively on a Mac (universal arm64 + x86_64), or with [osxcross](https://github.com/tpoechtrager/osxcross) on Linux | Xcode command line tools, or osxcross `target/bin` on `PATH` |

Platforms without a toolchain are skipped with a message. `dist/SHA256SUMS`
lists the checksums of the archives. To publish, attach the files from
`dist/` to a GitHub release, e.g.
`gh release create v1.5.0 dist/*.zip dist/SHA256SUMS`. The installers find
the archive for each OS by its name ending (`-linux-x86_64.zip`,
`-windows-x64.zip`, `-macos-universal.zip`), so keep the names `build.sh`
gives them.

## Test tools

`build/mock_host` (Linux/macOS) is a tiny OpenFX host that loads the built
`.ofx`, runs describe / getFramesNeeded / isIdentity / render over a fake
clip with a cut and writes raw rgb24 frames. It is what the plugin is tested
with:

```sh
ffmpeg -f lavfi -i "smptehdbars=s=640x360:r=30:d=1.2" -pix_fmt rgb24 -f rawvideo from.rgb
ffmpeg -f lavfi -i "testsrc2=s=1280x720:r=30:d=1.2,crop=640:360:x='100+t*300':y='90+t*60'" \
       -pix_fmt rgb24 -f rawvideo to.rgb
./build/mock_host build/Datamosh.ofx.bundle/Contents/Linux-x86-64/Datamosh.ofx \
    from.rgb to.rgb 640 360 36 out.rgb
ffmpeg -f rawvideo -pix_fmt rgb24 -s 640x360 -r 30 -i out.rgb out.mp4
```

Any parameter can be set after the view argument as `name=value`, e.g.
`... out.rgb 0 recoverMode=4 recoverStart=0.4`. `MOCK_PURGE=1` drops the
plugin's cache before every frame (like scrubbing), so the output can be
compared with a normal sequential run; `MOCK_PARAMS=1` prints each
parameter's group and enabled state and exits. Other switches emulate what
real hosts do differently: `MOCK_THREADS=n` provides the multithread suite
with n threads, `MOCK_SCALE=0.5` renders at a render scale (output scaled
back up), `MOCK_PAD=n` gives source images an n-pixel border beyond the
output's bounds, `MOCK_OFFSET=n` moves every image's origin to (n, n),
`MOCK_NO_TEMPORAL=1` reports a host without temporal clip access. The host
prints the total and average time spent in render.

`MOCK_KEYS=cutMarker=1010,1030` gives a parameter keyframes at those times
(the fake clip starts at frame 1000, the cut in it is at 1020).
`MOCK_NO_KEYAPI=1` makes the host refuse keyframe queries (Keyframes mode
then finds no cut). `MOCK_PRESETS=1` runs the preset buttons: saves named
presets, picks, loads and deletes them, prints the values and the Saved
Presets menu after each step and exits; point `OFX_DATAMOSH_PRESETS` at a
scratch file so your own presets stay untouched. `MOCK_FIXED_CHOICES=1`
emulates a host that keeps the menus it got when the plugin was described
(new presets then appear only after a restart); `MOCK_NO_PROPRESET=1` a
host that cannot clear a menu but lets entries be rewritten and added.
Messages the plugin shows through the message suite are printed as
`[message ...]`.

`build/mosh_test` runs only the algorithm core on two raw clips.
`MOCK_FLOAT=1` makes `mock_host` write the raw float RGBA output instead of
rgb24, for exact comparisons between builds or devices (add
`renderDevice=1` / `renderDevice=3` to force CPU / OpenCL).

## GPU kernels

The GPU path (`src/mosh_gpu.cpp`, `src/gpu_backend.cpp`) runs the
per-pixel stages of `mosh_core.h` as kernels in `src/mosh_kernels.cl`; the
per-block decisions stay on the CPU. One source serves both APIs: OpenCL
drivers compile it at run time, and for CUDA it is precompiled to PTX
(`src/mosh_kernels.ptx`, checked in) that the driver compiles for the card.
Both are embedded in the plugin, and the CUDA and OpenCL drivers are loaded
at run time, so building needs no GPU SDK. The kernels do the same float
operations in the same order as the CPU code, with no fused multiply-adds
and correctly rounded division and square roots, so the results are
identical; keep it that way when changing either side.

After editing `mosh_kernels.cl`, regenerate the PTX with NVRTC from CUDA 11
or newer (a CUDA toolkit, or the `nvidia-cuda-nvrtc-cu11` pip package; set
`PTXAS` to also check the result for sm_50 to sm_90):

```sh
NVRTC_INC=/usr/local/cuda/include NVRTC_LIB=/usr/local/cuda/lib64 tools/gen_ptx.sh
```

The PTX carries the SHA-256 of the `.cl` it was made from; CMake warns when
they do not match.

`build/gpu_selftest [auto|cuda|opencl] [width height]` renders a synthetic
mosh through every recovery mode and several settings on the CPU and on the
GPU, compares the motion fields and pictures bit for bit and prints
`RESULT: OK` or the first differences, plus timings. On a machine without a
GPU, an OpenCL CPU driver such as pocl runs the OpenCL path
(`gpu_selftest opencl`). `OFX_DATAMOSH_GPU_DEVICE` picks a device by name
here as in the plugin.

## Layout

```
src/mosh_core.h          host-independent algorithm (motion search, mosh step, recovery)
src/datamosh_plugin.cpp  OpenFX glue: describe, params, cut detection, caches, render
src/mosh_gpu.cpp         GPU path: frame and result caches on the device, the GPU stages
src/gpu_backend.cpp      CUDA driver API and OpenCL backends, loaded at run time
src/mosh_kernels.cl      GPU kernels for CUDA and OpenCL; mosh_kernels.ptx is generated from it
tools/gen_ptx.sh         regenerates mosh_kernels.ptx with NVRTC
tools/gpu_selftest.cpp   compares the GPU path with the CPU bit for bit
tools/mosh_test.cpp      offline harness for the core
tools/mock_host.cpp      minimal OpenFX host for end-to-end plugin tests
packaging/Info.plist     bundle metadata
install.sh / install.ps1 install a local build or the latest release (SHA-256 checked)
build.sh                 release archives for Linux, Windows and macOS into dist/
cmake/                   toolchain files for MinGW-w64 and osxcross cross builds
```
