# User guide

[Back to README](../README.md)

* [Parameters](#parameters)
* [Recovery modes](#recovery-modes)
* [Mosh styles](#mosh-styles)
* [Repeat (one-clip mosh)](#repeat-one-clip-mosh)
* [Marking cuts with keyframes](#marking-cuts-with-keyframes)
* [Presets](#presets)
* [Looks to start from](#looks-to-start-from)
* [Performance](#performance)
* [Reading the Diagnostics view](#reading-the-diagnostics-view)

## What the plugin does

Real datamosh is a decoder applying P-frame motion vectors and residuals to
the wrong reference picture because the I-frame is gone. The plugin does the
same thing in software, per frame:

1. Finds the cut (the frame where a keyframe would have been), either
   automatically by the jump between consecutive frames or from a frame
   number you give.
2. Takes the last frame before the cut as the reference picture.
3. Estimates block motion between consecutive frames of the new shot the way
   an encoder does: hierarchical block matching refined to half a pixel,
   median-smoothed vector field, textureless blocks inherit their
   neighbours' vector.
4. Makes the encoder's intra decision for every block: where predicting a
   block of the new shot costs more than coding it from scratch, the block
   is coded intra and shows the new shot as it is. This starts on the first
   frame after the cut, so the new shot replaces the old one bit by bit,
   just as in a real stream with a deleted keyframe.
5. Drags the reference (and every fresh block) along the vectors with
   interpolation and adds each step's prediction residual, which carries
   the new shot's changes in. Optionally decodes every P-frame several times
   (duplicated P-frames, the "bloom").
6. Heals the stream by the end of the mosh: by default the way a real one
   heals (more and more intra blocks, fresh regions growing), or with one
   of the stylised recovery modes.

## Parameters

The parameters are split into groups in the inspector. Parameters that do
nothing with the current settings are greyed out (Cut Frame in Auto mode,
Dark Areas First outside Luma-keyed refresh, and so on). The first group,
**Presets**, is closed by default; it saves and loads your own settings
(see [Presets](#presets)).

The menu entries use codec terms. Version 1.4 had only three recovery
modes, called Block refresh, Dissolve and Hard cut; they are now Random
intra refresh, Crossfade and IDR frame. The stored values did not change, so
1.4 projects open with the same settings.

**Cut Detection**

| Parameter | Default | Meaning |
|---|---|---|
| Cut | Auto (scene-change detection) | Auto looks back up to Mosh Length frames for a hard cut; Manual uses Cut Frame; Keyframes uses the keyframes of Cut Markers, see [Marking cuts with keyframes](#marking-cuts-with-keyframes). Only the setting of the chosen way is shown below it (hosts that cannot hide parameters grey the others out). |
| Cut Threshold | 0.12 | Auto mode: frame-to-frame difference that counts as a cut. |
| Cut Frame | 0 | Manual mode: first frame of the new shot (Repeat: the first repeated frame), in the clip's own frame numbers (Diagnostics shows `T`). |
| Cut Markers | 0 | Keyframes mode: every keyframe of this parameter is a cut (Repeat: the start of a repeat). The value does nothing; only where the keyframes are matters. |

**Mosh**

| Parameter | Default | Meaning |
|---|---|---|
| Mosh Type | Transition | Transition: the new shot's keyframe is missing, the old picture follows the new shot's motion. Repeat: the last P-frames before the cut frame are decoded over and over, a mosh of one clip. See [Repeat](#repeat-one-clip-mosh). |
| Repeat Frames | 1 | Repeat: how many of the last P-frames are looped. |
| Mosh Style | Intra + half-pel | See [Mosh styles](#mosh-styles). |
| Mosh Length | 30 | Frames from the cut the mosh lasts, recovery included. |
| Residual Strength | 1.0 | 0 = pure smear of the old frame, 1 = codec-accurate, >1 overdrive. |
| New Data Rate | 0.6 | How readily blocks of the new shot are coded intra (fresh) instead of predicted, from the first frame after the cut. 0 = never, about 0.5 behaves like Xvid, higher replaces the old picture faster. |
| Bloom (P-frame Repeat) | 1 | Decode each P-frame N times. 2-4 gives the melting, over-saturated look. |
| Motion Scale | 1.0 | Multiplies vectors. >1 exaggerates, <0 inverts. |
| Clamp Levels | on | Keep RGB in 0..1 like an 8-bit decoder. |

**Motion Estimation**

| Parameter | Default | Meaning |
|---|---|---|
| Block Size | 16x16 | Macroblock size in px. Bigger = chunkier and faster. |
| Search Range | 48 | Max motion per step, px. Raise for very fast pans. |
| Half-pixel Motion | on | Half-pixel vectors with interpolation, like MPEG-4 and H.264; the smear softens as it travels. |
| Intra Threshold | 0.35 | Extra, absolute intra rule: blocks predicted worse than this always show the new picture. 1 = off. |
| Smooth Vectors | on | Median filter on the vector field. |
| Fill Flat Areas | on | Textureless blocks take their neighbours' motion so flat regions flow too. |

**Recovery**

| Parameter | Default | Meaning |
|---|---|---|
| Recovery | Encoder intra decisions | How the real picture comes back, see [Recovery modes](#recovery-modes). |
| Recovery Start | 0.8 | Fraction of the mosh at which the real picture starts to return. Not used by Encoder intra decisions, which heals over the whole mosh. |
| Recovery Curve | Linear | Pace of the recovery: Linear, Smooth (slow-fast-slow), Ease in (slow start), Ease out (slow end). |
| Softness | 0.5 | How gradual the recovery is; what exactly it softens depends on the mode. |
| Dark Areas First | off | Luma-keyed refresh only: reveal dark areas first. |
| Direction | Left to right | Intra refresh sweep and Slice displacement only: where the new shot comes in from (left, right, top, bottom, from the centre, to the centre). |
| Seed | 0 | Random refresh order, the ragged sweep edge, the slices, and the random intra blocks of Encoder intra decisions. |

**Performance & Debug**

| Parameter | Default | Meaning |
|---|---|---|
| Jump Cache (MB) | 1024 | RAM for checkpoints, pictures of the mosh saved every few frames so jumps and scrubbing continue from the nearest one. Shared by all Datamosh effects; 0 turns them off. Does not change the picture. See [Performance](#performance). |
| Render Device | Auto | Where the mosh is computed: Auto (an NVIDIA GPU through CUDA, else another GPU through OpenCL, else the CPU), CPU, CUDA or OpenCL. The picture is the same on every device. See [Performance](#performance). |
| View | Result | "Motion vectors" shows the field used for the frame (x -> red, y -> green, intra = white). "Diagnostics" overlays numbers. |

## Recovery modes

![recovery modes](recovery.jpg)

*Frames 14, 18, 21, 24 and 27 of a 30-frame mosh, Recovery Start 0.4
(Encoder intra decisions ignores it), Softness 0.5, other settings at their
defaults.*

| Mode | What happens | Softness controls |
|---|---|---|
| Encoder intra decisions | The stream heals the way a real one does, over the whole mosh from the cut on: the encoder codes more and more blocks intra, regions that already show the new shot grow outwards, and a few blocks in static areas are refreshed at random. Whatever is still stale fades in over the last 15%. Recovery Start is not used; Recovery Curve sets the pace. | - |
| Motion-adaptive intra refresh | The stream recovers inside the mosh, like an intra-refresh encoder. Regions where the new shot moves or changes come back first, whole areas at a time; static areas stay moshed longest. Refreshed blocks keep being decoded as P-blocks, so the smear around them keeps flowing in until it washes out. | how long the smear lingers in refreshed blocks |
| High-frequency first | The high band of the new shot (edges, texture) appears first over the smeared low band (colours) of the old one; then the low band follows. The smoothest, most "one picture turning into another" option. | how long the old colours stay after the shapes arrive |
| Low-frequency first | The reverse of High-frequency first: the low band (colours) of the new shot washes in under the smeared detail of the old one, then the detail follows. | how long the smeared detail stays after the colours arrive |
| Residual-ordered refresh | Areas with the smallest residual between the mosh and the new shot settle first; the worst mismatches stay moshed longest. | width of the soft edge |
| Luma-keyed refresh | The new shot shows through its bright areas first (or dark ones, with Dark Areas First). | width of the soft edge |
| Intra refresh sweep | A refresh front sweeps across the frame in the chosen Direction, like periodic intra refresh, with a ragged edge one macroblock deep. | width of the soft edge |
| Slice displacement | The frame is cut into slices 1-4 macroblocks tall (or wide, for top/bottom directions). Each slice shifts the new shot in and pushes the mosh out, on its own schedule. With the centre directions neighbouring slices shift in opposite ways. | how long each slice takes to shift in |
| Random intra refresh (1.4: Block refresh) | Macroblocks are refreshed in random order. | how long each block takes to fade in (0 = they pop) |
| Crossfade (1.4: Dissolve) | Linear blend from the moshed picture to the real one. | - |
| IDR frame (1.4: Hard cut) | The real picture as soon as recovery starts, like the next keyframe. | - |

All modes except Encoder intra decisions start at Recovery Start. New Data
Rate works in every mode, so the new shot starts coming in at the cut with
any of them; set it to 0 for a pure smear until recovery.

To get the 1.4 look back exactly: Mosh Style P-only, full-pel, Recovery
Random intra refresh, Softness 0 (Residual Strength 0.6, which that style
sets).

## Mosh styles

| Style | What it does | Defaults it brings |
|---|---|---|
| Intra + half-pel (new shot from the cut) | Encoder-style intra/inter decisions per macroblock (New Data Rate) and half-pixel motion compensation: fresh blocks of the new shot appear from the first frame after the cut and travel with the motion, so the two shots merge gradually. | Residual Strength 1.0, Recovery Encoder intra decisions |
| P-only, full-pel (smear until recovery) | No intra decisions besides Intra Threshold, whole-pixel motion: the old picture is smeared by the new shot's motion and the new shot comes back during recovery, as in versions before 1.5. New Data Rate and Half-pixel Motion are greyed out. | Residual Strength 0.6, Recovery Motion-adaptive intra refresh |

When you switch style, Residual Strength and Recovery move to the new
style's defaults, but only if they were still at the old style's defaults;
values you changed stay. Every Recovery mode works with both styles.

## Repeat (one-clip mosh)

With **Mosh Type** on **Repeat** the plugin imitates the other classic
datamosh: duplicated P-frames. From the cut frame on (here it is the frame
where the repeat starts, set with Cut Frame or Cut Markers) the decoder
decodes the last P-frames before it over and over instead of the real ones.
The picture stays on the last real frame and keeps flowing along the motion
of those frames: what moved there smears further and further the same way,
and every repeat adds the same residual again, so the colours bloom and
burn. No second shot is needed.

* **Repeat Frames** sets how many of the last P-frames are looped. 1 repeats
  the motion of the frame just before the start; 3-8 loop a short stretch
  of motion, which looks less uniform. Up to 8 looped frames are kept in
  memory; longer loops fetch their frames from the host again for every
  step, which is slower.
* The motion vectors stay where they were in the looped frames: the picture
  flows where things moved and holds still elsewhere, as in a real stream.
* Mosh Style matters as in a transition. With Intra + half-pel, blocks the
  encoder coded intra in the looped frames are pasted in again on every
  repeat, so parts of the picture hold still; P-only, full-pel smears
  everything.
* Residual Strength sets how much the repeated residual piles up (try
  0.3-0.6 if the colours burn out too fast); Bloom speeds up the flow.
* By the end of Mosh Length the live clip is back. Recovery brings it in as
  in a transition: the stylised modes blend the live clip over the repeat,
  Encoder intra decisions heals the repeat with growing blocks of the live
  clip, Motion-adaptive intra refresh refreshes first where the live clip
  moves.
* Auto cut detection works too: the repeat then starts at a hard cut and
  the old shot's last motion carries on over the new shot.
* A repeat needs the frame before each looped P-frame. A start closer to the
  beginning of the clip than Repeat Frames + 1 frames loops fewer frames;
  on the clip's first two frames there is nothing to repeat.
## Marking cuts with keyframes

Auto finds every hard cut in the clip by itself. When you want to pick the
cuts yourself (only some of them, or a cut Auto does not see), set **Cut**
to **Keyframes (Cut Markers)** and mark the cuts with keyframes of **Cut
Markers**:

1. Move the playhead to the first frame of the new shot.
2. Click the keyframe button next to Cut Markers. Its value does nothing,
   only where the keyframes are matters. It is a number so that the host
   can show its keyframes in its keyframe views.
3. Repeat for every cut you want moshed.

To move a cut, drag its keyframe; to drop a cut, delete its keyframe. Only
marked cuts are moshed, any others in the clip are left alone. A keyframe
on the very first frame of the clip is ignored, because there is no old
picture before it. Diagnostics shows `KEYS n`, the number of keyframes the
plugin sees.

Earlier development builds kept Cut Markers in other parameters (a
one-entry menu, whose keyframes Resolve does not show, then a fractional
number). Projects made with them keep their cuts; the first time any
setting of the effect is changed, those keyframes move to Cut Markers.

Some hosts do not tell plugins where keyframes are. There Diagnostics shows
`KEYS UNKNOWN` and Keyframes mode finds no cut; use Auto or Manual frame
instead.

## Presets

DaVinci Resolve has no way to save the settings of an OpenFX effect as a
preset, so the plugin has its own. Open the **Presets** group:

* **Save**: type a name in **Preset Name** and press **Save Preset**. Saving
  under a name that already exists replaces that preset.
* **Load**: pick the preset in **Saved Presets** (sorted by name) and press
  **Load Preset**. This works on any clip, in any project.
* **Delete**: pick the preset in **Saved Presets** and press **Delete
  Preset**. It is removed from the presets file and from the menu.

Picking a preset in Saved Presets also puts its name into Preset Name, so
Save Preset then updates the picked preset. There is no limit on the number
of presets.

A preset holds every setting, including Cut mode and Cut Threshold, except
Cut Frame and the Cut Markers keyframes (those belong to one clip) and View.
The menu of an effect that is already applied catches up with presets saved
or deleted elsewhere (on another clip, in another project) when you open
it and pick an entry, or when you save or delete a preset there. Some hosts
read the menu only once per session; there a new or deleted preset shows in
the menu after the host restarts, and the plugin says so when you save or
delete.

The presets are kept in `presets.ini` in your user settings folder, which
you can copy to another machine or edit by hand:

| OS | File |
|---|---|
| Windows | `%APPDATA%\ofx-datamosh\presets.ini` |
| macOS | `~/Library/Application Support/ofx-datamosh/presets.ini` |
| Linux | `~/.config/ofx-datamosh/presets.ini` (or `$XDG_CONFIG_HOME/ofx-datamosh/`) |

The environment variable `OFX_DATAMOSH_PRESETS` overrides the path.

Without the plugin's presets, Resolve itself can still carry settings over:
on the Edit page copy the clip, then on the target clip use **Paste
Attributes** (Alt+V) and tick Plugins; on the Color page save the node as a
still or PowerGrade.

## Looks to start from
* Melting smear: Mosh Style P-only, full-pel, Bloom 2-4, Residual 0.4-0.8,
  Motion Scale 1-1.5.
* Realistic mosh, like a broken stream: the defaults. Raise New Data Rate
  for a faster hand-over, lower it (0.3-0.4) and set Recovery Curve to
  Ease in for a long smear that heals late.
* Long, flowing transition between shots: Recovery Motion-adaptive intra
  refresh or High-frequency first, Recovery Start 0.4-0.5, Recovery Curve
  Smooth.
* Pure liquid smear of the old picture, no new colours: Residual 0,
  New Data Rate 0, Intra Threshold 1.
* Short glitch: Mosh Length 12, Recovery Start 0.6, Recovery IDR frame.
* Glitchy hand-off: Recovery Slice displacement, Softness 0.2-0.4, Block
  Size 32x32.

## Performance

The first frame after the cut is cheap; frame *k* of the mosh needs the
motion of *k* frame pairs. The plugin caches the previous frame's result, so
playing or rendering forward costs one motion step per frame.

While a mosh renders, the plugin also keeps a checkpoint (its picture so
far) every few frames in RAM. Scrubbing backwards or jumping into a part of
the mosh rendered before continues from the nearest checkpoint before the
frame, a few steps instead of the whole chain. Jumping past everything
rendered so far still runs the steps from the last checkpoint (or the cut)
up to the frame. A 1080p checkpoint takes 33 MB, a UHD one 133 MB. They
share **Jump Cache** (1024 MB by default) across all Datamosh
effects, at least 8 frames apart and further apart when a long mosh would
not fit; when the memory is full, the least recently used go first.

Every frame is computed from every frame pair since the cut, so a frame
looks the same however you got to it. (Earlier versions had a Max Motion
Steps cap that sampled long moshes at a coarser stride, which made such
frames jitter; it is gone, and projects that set it load with the value
ignored.)

Each source frame is fetched from the host once per render and the frame
before it is kept from the previous render, so in-order playback asks the
host for one new frame per frame. Hosts that render several frames at the
same time during playback, like Resolve, get the same: each frame waits for
the one before it if that is still being rendered, and continues from it.

**GPU.** With Render Device on Auto the motion search and the mosh steps run
on the GPU when there is one: an NVIDIA card through CUDA, AMD, Intel or
other cards through OpenCL. The GPU follows the CPU code operation for
operation, so the frames are bit for bit the same as on the CPU, and you can
switch devices in the middle of a project. Nothing has to be installed beyond
the normal graphics driver. The plugin uses its own GPU context next to
Resolve's and keeps the frames of the current mosh in GPU memory (roughly
0.3 GB for 1080p, 1 GB or a bit more for UHD). When the GPU cannot be used (no driver, too little
memory, an error) the frame is rendered on the CPU instead; after three
failed frames in a row the GPU is left alone until the plugin is reloaded.
The Diagnostics line `DEV` shows which device rendered the frame and, for
the CPU, why the GPU was not used. The Motion vectors view is always drawn
on the CPU. With several GPUs, set the environment variable
`OFX_DATAMOSH_GPU_DEVICE` to part of a device name (e.g. `4090`) before
starting the host to pick one.

Tips if it is still slow:

* Block Size 32x32 is about 4x faster than 16x16 and looks chunkier, often
  better.
* For long moshes at UHD, raise Jump Cache so the checkpoints sit
  closer together and jumps take fewer steps.
* In Resolve, render the clip in place (right-click > Render in Place) once
  you like it.

## Reading the Diagnostics view

Set **View** to **Diagnostics** on a frame just after the cut:

* `CUT n` is the detected cut frame; `NO CUT` means nothing within Mosh
  Length frames exceeded Cut Threshold. `SCAN MAX DIFF` is the largest jump
  seen: if it is below `THR`, lower Cut Threshold. `REPEAT n` (Mosh Type
  Repeat) is the number of P-frames looped, fewer than Repeat Frames near
  the start of the clip.
* `PAIR DIFF` is the difference between the two frames of the last motion
  step. Near 0.0000 means the host returned the same frame for different
  times (no temporal access), so there is no motion to work with.
* `MV` is the mean motion vector length in px. `CACHED n` / `CHECKPOINT n`
  / `FULL` says whether the frame continued from the cached result of frame
  n, from the checkpoint at frame n, or ran the chain from the cut.
* `Q` is the recovery progress (0..1, after the Recovery Curve), `REC` the
  recovery mode number (0-10 in menu order), `REFR` the share of blocks
  Motion-adaptive intra refresh has already refreshed (Encoder intra
  decisions: blocks that already carry the new shot), in percent. `INTRA`
  is the share of blocks coded intra in this frame.
* `DEV` is the device that rendered the frame: `CUDA` or `OPENCL` with the
  GPU name, or `CPU`, followed by the reason when a GPU was asked for but
  not used.
