# OFX Datamosh

An OpenFX filter that fakes a **datamosh** across a cut, without touching the
video stream. Drop it on a clip that contains a cut (in DaVinci Resolve: a
compound clip made of two clips), and the frames after the cut melt the way a
video with a deleted keyframe does.

![sample](docs/sample.png)

Works in DaVinci Resolve (Linux, Windows, macOS) and should load in any
OpenFX 1.4 host that gives filters access to neighbouring frames.

## What it does

Real datamosh is a decoder applying P-frame motion vectors and residuals to
the wrong reference picture because the I-frame is gone. The plugin does the
same thing in software, per frame:

1. Finds the cut (the frame where a keyframe would have been), either
   automatically by the jump between consecutive frames or from a frame
   number you give.
2. Takes the last frame before the cut as the reference picture.
3. Estimates block motion between consecutive frames of the new shot the way
   an encoder does: hierarchical block matching, median-smoothed vector
   field, textureless blocks inherit their neighbours' vector, hopeless
   blocks become intra blocks.
4. Drags the reference along those vectors and adds each step's prediction
   residual, which bleeds the new colours in. Optionally decodes every
   P-frame several times (duplicated P-frames, the "bloom").
5. Brings the real picture back: macroblocks refresh randomly like an
   intra-refresh stream, or a dissolve, or a hard cut.

## Install

Grab the archive for your OS from the
[latest release](../../releases/latest) and unpack it. (If your OS has no
archive in the release, build from source, see below.) Inside is
`Datamosh.ofx.bundle`. Copy the whole bundle folder into the system OpenFX
plugin directory, then restart the host.

| OS | Plugin directory |
|---|---|
| Linux | `/usr/OFX/Plugins/` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\` |
| macOS | `/Library/OFX/Plugins/` |

Create the directory if it does not exist. Copying needs administrator
rights on all three systems.

**Linux**

```sh
sudo mkdir -p /usr/OFX/Plugins
sudo cp -r Datamosh.ofx.bundle /usr/OFX/Plugins/
```

**Windows** (PowerShell as Administrator)

```powershell
New-Item -ItemType Directory -Force "C:\Program Files\Common Files\OFX\Plugins" | Out-Null
Copy-Item -Recurse -Force .\Datamosh.ofx.bundle "C:\Program Files\Common Files\OFX\Plugins\"
```

**macOS**

```sh
sudo mkdir -p /Library/OFX/Plugins
sudo cp -R Datamosh.ofx.bundle /Library/OFX/Plugins/
# the binary is not notarised; clear the quarantine flag so Resolve may load it
sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/Datamosh.ofx.bundle
```

If you built from source, `install.sh` (Linux/macOS) or `install.ps1`
(Windows) does the same. Without arguments they pick the newest build for
the current OS from the unpacked archive next to them, `build-release/`,
`build/` or the archives in `dist/`, and refuse bundles built for another OS.
You can also pass a bundle or a zip: `sudo ./install.sh dist/ofx-datamosh-v1.4-linux-x86_64.zip`,
`.\install.ps1 -Path <bundle-or-zip>`. Both take a target directory
(`--dest DIR` / `-Dest DIR`) and show help with `--help` / `Get-Help .\install.ps1`.

## Using it in DaVinci Resolve

1. Put the two clips next to each other on the timeline (a plain cut).
2. Select both, right-click, **New Compound Clip**.
3. Effects Library > OpenFX > Filters > **Datamosh**, drop it on the
   compound clip.
4. **Cut: Auto detect** finds the cut by the jump between frames. Set
   **Mosh Length** (frames after the cut) and tune the look.

Why a compound clip: the filter needs to see both sides of the cut as one
clip. A compound clip also works with several cuts inside; every cut gets
moshed.

Why not a transition: Resolve does not give OpenFX transitions access to
neighbouring frames, and a datamosh is nothing but neighbouring frames.

### Performance

The first frame after the cut is cheap; frame *k* of the mosh needs the
motion of *k* frame pairs. The plugin caches the previous frame's result, so
playing or rendering forward costs one motion step per frame. Scrubbing
backwards or jumping into the middle of a mosh rebuilds the chain from the
cut (bounded by **Max Motion Steps**). Tips if it is still slow:

* Block Size 32 is about 4x faster than 16 and looks chunkier, often better.
* Lower Max Motion Steps (24) to bound the worst case when scrubbing.
* Render the clip in place (right-click > Render in Place) once you like it.

## Parameters

| Parameter | Default | Meaning |
|---|---|---|
| Cut | Auto detect | Auto looks back up to Mosh Length frames for a hard cut; Manual uses Cut Frame. |
| Cut Frame | 0 | Manual mode: first frame of the new shot, in the clip's own frame numbers (Diagnostics shows `T`). |
| Cut Threshold | 0.12 | Auto mode: frame-to-frame difference that counts as a cut. |
| Mosh Length | 30 | Frames after the cut the mosh lasts, recovery included. |
| Block Size | 16 | Macroblock size in px. Bigger = chunkier and faster. |
| Search Range | 48 | Max motion per step, px. Raise for very fast pans. |
| Motion Scale | 1.0 | Multiplies vectors. >1 exaggerates, <0 inverts. |
| Residual Strength | 0.6 | 0 = pure smear of the old frame, 1 = codec-accurate, >1 overdrive. |
| Bloom (P-frame Repeat) | 1 | Decode each P-frame N times. 2-4 gives the classic melting, over-saturated look. |
| Intra Threshold | 0.35 | Blocks predicted worse than this show the new picture directly. 1 = off. |
| Smooth Vectors | on | Median filter on the vector field. |
| Fill Flat Areas | on | Textureless blocks take their neighbours' motion so flat regions flow too. |
| Recovery Start | 0.8 | Fraction of the mosh at which the real picture starts to return. |
| Recovery | Block refresh | Block refresh / Dissolve / Hard cut. |
| Max Motion Steps | 48 | Cap on frame pairs analysed per frame when the cache is cold. |
| Clamp Levels | on | Keep RGB in 0..1 like an 8-bit decoder. |
| Seed | 0 | Changes which blocks refresh first. |
| View | Result | "Motion vectors" shows the field used for the frame (x -> red, y -> green, intra = white). "Diagnostics" overlays numbers. |

Looks to start from:

* Classic melt: Bloom 2-4, Residual 0.4-0.8, Motion Scale 1-1.5.
* Pure liquid smear of the old picture, no new colours: Residual 0,
  Intra Threshold 1.
* Short glitch: Mosh Length 12, Recovery Start 0.6, Recovery Hard cut.

## Reading the Diagnostics view

Set **View** to **Diagnostics** on a frame just after the cut:

* `CUT n` is the detected cut frame; `NO CUT` means nothing within Mosh
  Length frames exceeded Cut Threshold. `SCAN MAX DIFF` is the largest jump
  seen: if it is below `THR`, lower Cut Threshold.
* `PAIR DIFF` is the difference between the two frames of the last motion
  step. Near 0.0000 means the host returned the same frame for different
  times (no temporal access), so there is no motion to work with.
* `MV` is the mean motion vector length in px. `CACHED` / `FULL` says
  whether the frame reused the previous result or rebuilt the chain.

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
sudo ./install.sh
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

Platforms without a toolchain are skipped with a message. To publish, attach
the files from `dist/` to a GitHub release, e.g.
`gh release create v1.5.0 dist/*.zip`.

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

`build/mosh_test` runs only the algorithm core on two raw clips.

## Layout

```
src/mosh_core.h          host-independent algorithm (motion search, mosh step, recovery)
src/datamosh_plugin.cpp  OpenFX glue: describe, params, cut detection, caches, render
tools/mosh_test.cpp      offline harness for the core
tools/mock_host.cpp      minimal OpenFX host for end-to-end plugin tests
packaging/Info.plist     bundle metadata
install.sh / install.ps1 copy the built bundle into the system plugin directory
build.sh                 release archives for Linux, Windows and macOS into dist/
cmake/                   toolchain files for MinGW-w64 and osxcross cross builds
```

## License

MIT
