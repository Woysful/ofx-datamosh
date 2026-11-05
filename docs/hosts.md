# Hosts

[Back to README](../README.md)

The plugin is a plain OpenFX 1.4 filter (Filter context, RGBA or RGB, 8-bit,
16-bit or float, CPU rendering) with no host-specific code. It needs one
thing from the host: **temporal clip access**, i.e. a filter may fetch frames
other than the one being rendered. A host that reports it does not support
this gets a clean "missing host feature" error instead of a broken effect.

| Host | Status |
|---|---|
| DaVinci Resolve (Linux, Windows, macOS) | The main target, used in practice. |
| Natron 2.5 (Linux) | Tested headless (Read > Datamosh > Write) with both styles, several recovery modes, a manual cut and the motion-vector view: the same frames as the reference test host, up to 8-bit rounding. Natron honours the "prefers sequential render" hint, so each frame costs one motion step. |
| Nuke, Fusion Studio, Flame, Baselight, Vegas, HitFilm and other OpenFX hosts | Not tested. Should work when the host gives filters temporal access; please open an issue if it does not. |

Things that differ between hosts and are handled:

* Images whose bounds differ from the output's (padding, a different
  origin, negative row strides) are mapped onto the output's pixel grid.
* Proxy / draft renders (render scale below 1): Block Size and Search Range
  are scaled with the picture, so a proxy looks like a smaller copy of the
  full-resolution result rather than a chunkier one. It is still an
  approximation; final renders at full resolution are exact.
* Hosts that render several frames at once, out of order: the caches are
  shared safely and give the same result as playing in order, only slower.

The randomness in Encoder intra decisions is seeded by the absolute frame
number the host passes in, so the same clip placed at another time on a
timeline can show a slightly different pattern of random intra blocks.
