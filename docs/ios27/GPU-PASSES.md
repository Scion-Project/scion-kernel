# iOS 27 on the Galaxy S9: the two long GPU passes of a home-screen flick (24 November 2026)

Phone 2 (iOS 27.2.1, 24B156), the owner's root as found, the fork kernel with the charger guard, Mali-G72 MP18 at 572 MHz.
Follows [PACING.md](PACING.md): a heavy flick frame costs 25-30 ms of GPU and two fragment passes are about 18 ms of it.
M = measured on the phone, I = inferred, N = not checked. Every measurement was taken seven minutes after SpringBoard
started (after the boot's slow mode), with the console flicks of `msr.sh`.

**Result in one line: the two passes are named and their cost is located (in the draws, spread over every kind of draw), but
no pixel-exact change that makes them cheaper was found in this round. Nothing here changes what runs on a phone.**

## 1. What the two passes are (M)

Read with the pass log (`SWEET_MTL2_PASSLOG`, `tools/ios27/metal2/PassLog.h`; first run on a phone here) in backboardd during
the flicks, matched by order and by the size of the tiler job before each to `sgu_dump`'s chains. A heavy frame is one command
buffer of 15 render encoders:

| # | Target | Draws | What | GPU |
|---|---|---|---|---|
| e0-e4 | 168x76 -> 46x22 -> 60x36 x2 -> 222x130, BGRA8 private | 1-2 each | a small blurred element: `downsample_4`, `narrow_blur_15` x2, upsample | ~76 us each |
| e5 | the drawable (below) | 3 | fill, the wallpaper (920x1208 BGRA8 over a buffer, 1 level), one more image | 1.7-2.3 ms |
| e6-e11 | 414x404, then 480x464 BGRA8 with 5 levels | 1-2 each | the glass backdrop: a copy of the drawable and `variable_blur_downsample` into each level | ~76 us each |
| **e12** | **the drawable** | **51-68** | **the page: `glass_background_cr_lph` x2, `glass_background_sdf_cr_lph` x3, the icons and labels (`TimgBvcm*`, `Timg*_Ialp`, `TimgA3Xhfn_Isrc`, `Tdkf*`, `Tmua*`, `Tcim*`, ...)** | **10.7-16.4 ms (mean 13.4)** |
| e13 | 174x174 BGRA8 | 1 | a copy of the drawable (the dock's backdrop) | ~0.2 ms |
| **e14** | **the drawable** | **35-49** | **the dock: `TbdsA3Xhf_Isup_Isrc` over the 174x174 copy, `Tdkf*`, `TimgBvcm*`, icons, `Tmua*`** | **5.9-7.8 ms (mean 6.4)** |

The drawable in e5, e12 and e14: color 0 = 828x1702 `BGRA8Unorm` over an IOSurface, loaded and stored; color 1 = 828x1702
`RGBA16Float` memoryless, don't-care load and store; color 2 = 828x1702 `R32Float` memoryless, don't-care. Color 1 is
CoreAnimation's offscreen group in tile memory (the `*S1*` pipelines draw into it, `*_IsrcM1` read it back); no pipeline
writes color 2. Blending is fixed-function, premultiplied over (one, one-minus-source-alpha) on color 0 (or 1). CoreAnimation
makes two samplers only: linear and nearest, no mip filter, anisotropy 1. Each long pass is one fragment chain: our layer
does not split it. 16 bytes a pixel in the tile buffer: 16x16 tiles (the G72's 8 KiB budget), transaction elimination off
(more than one target; it would not help a moving page anyway). In some boots and moments a frame has only the e12-like pass.

Which is which: e12 is the page of icons with their glass (the folder and widget backgrounds), e14 the dock (I, from the draws
and the 174x174 backdrop; the names are CoreAnimation's own).

## 2. Where the time goes (M, diagnostic builds only)

A diagnostic knob (kernel sysctl `kern.hakios.mtl_knob`, set from the USB console; the frontend reads it at each command
buffer) dropped or changed classes of draws within one boot, and `sgu_dump` (re-armed at each `sweetfb_trace_go`) gave the
chains of three heavy-flick stretches per setting. Means of the long (>= 8 ms) / medium (4-8 ms) fragment chains:

| Change (diagnostic: pictures wrong) | Long | Medium |
|---|---|---|
| none (D3 / V1b / V2b boots) | 13.4 / 12.7 / 11.6 | 6.4 / 6.0 / 5.6 |
| no `glass_background*` draws (5 draws) | 10.2 | 6.4 |
| no `*_Ialp` draws | 10.8 | 5.7 |
| no `TimgBvcm*` draws | 10.6 | 5.8 |
| no `Tdkf*` draws (6 draws) | 11.8 | 6.0 |
| no `Tmua*` draws | 12.9 | 5.0 |
| no `TimgA3Xhfn_Isrc` draws | 12.9 | 5.6 |
| no `TbdsA3Xhf`/`Tcim*` draws | 12.7 | 6.3 |
| no draw at all in a pass of 1 Mpx or more | none over 4 ms; the empty pass (load + store of the drawable) <= 1.8 ms | |
| blending off on every color | 13.8 | 6.0 |
| no writes to colors 1 and up | 13.1 | 6.8 |
| no writes to color 2 | 14.1 | 6.4 |
| every fragment sampler nearest (no filtering) | 12.9 (V2b: 11.6 without) | 5.3 |

And the shaders as PanVK compiles them (`BIFROST_MESA_DEBUG=shaderdb`, a cold-cache boot, 34 pipelines before the heat stop):
the icon and fill fragment shaders are 24-233 instructions, about 1-6 cycles a thread by the compiler's estimate, no spills;
the blur shaders 1,300-2,100 instructions. (The glass shaders were not reached.)

So (I):
- The cost is **in the draws**, spread over every kind: a glass draw about 640 us, an icon or label draw about 200-280 us.
  It is not a fixed cost of the pass (load, store, a flush, a split): an empty pass is under 2 ms.
- It is **not** the blending, **not** the writes of the tile-memory colors, **not** texture filtering, and **not** the shaders'
  arithmetic: an icon-sized quad (about 36,000 pixels) at 200 us is about 50-70 GPU core-cycles a pixel, against 1-6 cycles of
  arithmetic the compiler counts.
- **Not checked (N), the suspects left**: (1) the tile-buffer reads in rasterization order (CoreAnimation's shaders read colors
  back, framebuffer fetch; PanVK lowers them to tile loads that must wait for every earlier fragment of the pixel, so
  overlapping layers run one after another inside a tile and the texture latency is not hidden); (2) forward pixel kill off for
  every pipeline (`mesa26-panvk-ios.patch`: any rasterization-order pipeline; metal2vulkan marks every pipeline so), which
  leaves the opaque layers (`TimgBvcm*`, the wallpaper) shading pixels that later layers cover; (3) a per-tile, per-draw cost
  of the bin hierarchy (the tiler jobs of these passes are 0.7-1.2 ms for under 1,000 vertices); (4) texture fetch latency
  as such. Each needs its own diagnostic: (1) a PanVK build whose input-attachment loads count (or skip) per draw; (2) a PanVK
  build with forward pixel kill allowed for pipelines that read no color, checked for exact pixels; (3) `STARBITE_PANVK_BINS`
  (its `aa` value hung a boot before: needs care); (4) a frontend that binds 1x1 textures.

## 3. Checks on the way (M)

- **The fork head** (`scion/xnu-12377` 9bc42aa7 = a494fb4's tree, PR 69's last commit never run before): built (rc 0), booted
  on phone 2 with the found kernel's line and the guard (`sweetchg=2 ... the guard`), Wi-Fi associated, Bluetooth's kernel
  control up, no panic; flicks 28.4 fps, Settings list 21.0, open/close 18.8, big 62.3 C max: the same as the found kernel
  (29.1 / 21.7 / 13.2 in PACING.md's A0). Nothing to fix or revert.
- **The pass log costs nothing visible**: with it on, flicks 29.2 fps, Settings list 21.9, open/close 13.1.
- **Heat**: a boot whose backboardd pipeline cache was moved aside (to see every compile) reached 80.0 C 45 s after
  SpringBoard; a boot just after a root push reached 77.1-78.1 C in the same first minute. Both were sent to recovery to cool
  at once. Do not run a cold-cache boot again; watch the first minute of any boot.

## 4. Tools

- `tools/ios27/perf/sgu-dump-chains.py LOG MARKS`: the fragment chains of `sgu_dump` printed after each round's start in a
  marks file (`HH:MM:SS K<knob> R<round> start`), by knob: count and mean of the long (>= 8 ms), medium (4-8 ms) and short
  (1-4 ms) fragment chains, the fragment and tiler time summed.
- The diagnostic builds themselves are not in a repository: a frontend that drops or changes draws must never reach a root.
  What they did: the frontend reads `kern.hakios.mtl_knob` (a 3-line `SYSCTL_INT` with `CTLFLAG_KERN` so that the USB console
  can set it) at each command buffer; bits 1-128 skip draws by fragment-function name, 32 every draw of a pass of 1 Mpx or more,
  256 blending off, 512/1024 colors' write masks to zero, 2048 nearest samplers. The kernel also re-armed `sgu_dump` at each
  `sweetfb_trace_go` (one line in `sweet_fb_trace_go_sysctl`). Shader statistics: `BIFROST_MESA_DEBUG=shaderdb` in
  `mtl2-env` and backboardd's stderr reopened to a file (Mesa prints them to stderr); they print only at a compile, so only a
  cold cache shows them all (see Heat).
