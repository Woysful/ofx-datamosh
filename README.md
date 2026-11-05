# OFX Datamosh

An OpenFX filter that fakes a **datamosh** across a cut, without touching the
video stream. Drop it on a clip that contains a cut and the frames after the
cut melt the way a video with a deleted keyframe does. Or mosh a single clip:
from a frame you pick, its last P-frames repeat over and over and the picture
flows on along their motion (duplicated P-frames).

![sample](docs/sample.png)

Made for DaVinci Resolve; works in other OpenFX hosts that let a filter read
neighbouring frames ([Hosts](https://github.com/Woysful/ofx-datamosh/blob/main/docs/hosts.md)).

## How it works

The plugin does in software what a decoder does when the keyframe at a cut
is gone: it estimates block motion in the new shot like an encoder,
drags the last picture before the cut along those vectors, adds the
prediction residuals and codes some blocks intra, so the new shot takes over
bit by bit until the stream heals. The default look was tuned against a real
MPEG-4 ASP (Xvid-class) stream of the same clips with the keyframe deleted:

![real mosh vs plugin](docs/natural.jpg)

*Frames 2, 7, 12, 17, 22 and 27 after the cut, 30-frame mosh.*

## Install

```sh
# Linux / macOS (asks for your password through sudo)
curl -fsSL https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.sh | sh
```

```powershell
# Windows, in PowerShell started as Administrator
irm https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.ps1 | iex
```

Then restart the host. Releases have archives for Linux x86_64 and
Windows x64; on macOS build from source. Manual install, installer options
and uninstall: [docs/install.md](https://github.com/Woysful/ofx-datamosh/blob/main/docs/install.md).

## Quick start in DaVinci Resolve

1. Put the two clips next to each other on the timeline (a plain cut).
2. Select both, right-click, **New Compound Clip**.
3. Effects Library > OpenFX > Filters > **Datamosh**, drop it on the
   compound clip.
4. The cut is found automatically. Set **Mosh Length** (frames after the
   cut) and tune the look.

The filter has to see both sides of the cut as one clip, hence the compound
clip; every cut inside it gets moshed. A transition would not work: Resolve
does not give OpenFX transitions access to neighbouring frames.

To mosh a single clip, drop the filter on it, set **Mosh Type** to
**Repeat** and **Cut** to **Manual frame** (or **Keyframes**), and set the
frame where the repeat starts.

## Main settings

| Setting | What it changes |
|---|---|
| Cut | Found automatically. Set it to **Keyframes** to mark the cuts yourself with keyframes of Cut Markers, which you can drag. |
| Mosh Type | **Transition** (deleted keyframe at a cut) or **Repeat**: from the cut frame on, the last P-frames before it are decoded over and over, so one clip melts by itself. **Repeat Frames** sets how many are looped. |
| Mosh Style | **Intra + half-pel**: the new shot comes in from the first frame after the cut, like a real stream. **P-only, full-pel**: the old picture is only smeared until recovery starts. |
| Mosh Length | How many frames after the cut the mosh lasts. |
| New Data Rate | How fast fresh blocks of the new shot replace the old picture. |
| Bloom (P-frame Repeat) | 2-4 gives the melting, over-saturated look. |
| Block Size | Bigger blocks look chunkier and render faster. |
| Recovery | How the real picture comes back: like a real encoder (default), or one of ten stylised modes. |
| Presets | Save your settings under a name and load them from a menu on any clip, in any project. |
| Render Device | Auto renders on an NVIDIA (CUDA) or other (OpenCL) GPU when there is one, with the same result as the CPU. |

Every parameter, the recovery modes with pictures, starting points for
different looks and the Diagnostics view are described in the
[user guide](https://github.com/Woysful/ofx-datamosh/blob/main/docs/guide.md).

## Documentation

* [User guide](https://github.com/Woysful/ofx-datamosh/blob/main/docs/guide.md): parameters, recovery modes, looks, performance, diagnostics
* [Install and uninstall](https://github.com/Woysful/ofx-datamosh/blob/main/docs/install.md)
* [Hosts](https://github.com/Woysful/ofx-datamosh/blob/main/docs/hosts.md): which OpenFX hosts work and what the plugin needs
* [Development](https://github.com/Woysful/ofx-datamosh/blob/main/docs/development.md): building, release archives, test tools

## Build from source

A C++17 compiler and CMake 3.16+; no other dependencies.

```sh
git clone https://github.com/Woysful/ofx-datamosh.git && cd ofx-datamosh
git clone --depth 1 https://github.com/AcademySoftwareFoundation/openfx.git third_party/openfx
./build.sh --install            # build for this OS and install
```

Windows with Visual Studio and cross builds:
[docs/development.md](https://github.com/Woysful/ofx-datamosh/blob/main/docs/development.md).

## License

MIT
