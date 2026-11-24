<!-- SPDX-License-Identifier: MIT-0 -->
# The memory-to-memory scaler on iOS 27 (Galaxy S9)

State, 24 November 2026: ****built and run on a phone (phone 1, 24 November): the service opens, publishes its capabilities and does the transfers
iOS asks of it; the camera preview's stitcher now draws the picture and it reaches the preview's image queue; the
viewfinder on the screen is still black**, because of a later break on the display side (below, "The phone test").**

iOS's camera preview, VideoToolbox's rotation sessions and CMCapture's `FigM2MController` expect a memory-to-memory scaler
and colour-space converter: on an iPhone, the IOService `AppleM2ScalerCSCDriver`, reached from user space through
`IOSurfaceAccelerator.framework`. The S9 had none. The stitcher of the camera preview then caps its up-scale at 0, asks for
a 0x0 source rectangle and draws nothing (CAMERA.md, "What the stitcher trace showed"). This page is the interface iOS
expects of that service, as read from iOS 27.2.1 (24B156), and the S9's own service that answers it.

Everything about iOS here was read from 24B156's files on the build machine (IOSurfaceAccelerator, VideoToolbox's
`vt_Rotate_Celeste` and CMImaging's `+[FigM2MController queryCapabilities]`, by disassembly): names, numbers and what they do,
in our own words. Nothing of Apple's is in this repository. "Read" below means read in those files; "measured" means seen on
a phone; "inferred" means neither, and says why.

## The interface iOS expects

### Finding and opening the service (read)

- `IOSurfaceAcceleratorCreate(allocator, options, &accelerator)` looks the service up with `IOServiceMatching("AppleM2ScalerCSCDriver")`,
  i.e. by **class name**. Two other names are used only in other cases: `AppleSRSDriver` when the options name a
  `Sharpener`, and `AppleM2ScalerParavirtDriver` when the process runs virtualised.
- It opens it with `IOServiceOpen(service, mach_task_self(), 0, &connect)` (type 0; the sharpener uses another type).
- It then calls selector 10 with a 0x18-byte structure (a property; a failure is printed and ignored).
- No service: `IOSurfaceAcceleratorCreate` returns `0xe00002c0` (kIOReturnNoDevice). That is what the S9 answered before
  (measured on 23 November: `trace: stitch: IOSurfaceAcceleratorCreate: -536870208`).

### The user client's methods (read)

| Selector | Kind | In | Out | What |
|---|---|---|---|---|
| 0 | struct | 0x40 bytes | none | capture a surface (`IOSurfaceAcceleratorCaptureSurface`) |
| 1 | struct | 0x1b0 bytes | none | **the transfer** (`TransformSurface`, `TransferSurface`, `BlitSurface` and their variants all end here) |
| 2 | scalar | none | none | abort transfers |
| 3 | scalar | none | none | abort captures |
| 4 | struct | 0x20 bytes | none | the caller's own filter coefficients (`SetCustomFilter`: six sizes and a pointer) |
| 5 | scalar | none | 1 | an id (`IOSurfaceAcceleratorGetID`) |
| 7 | struct | 8 bytes | none | the histogram (`GetHistogram`, with the capability `kSurfaceAcceleratorCapabilitiesHistogramValues`) |
| 8 | struct | 8 bytes | none | diagnostics |
| 9 | struct | 0x10 bytes | none | an estimate of a transform's cost |
| 10 | struct | 0x18 bytes | none | a property (`IOSurfaceAcceleratorSetProperty`; also once at create) |

`IOSurfaceAcceleratorGetRunLoopSource` registers a notification port of type 0 (`IOConnectSetNotificationPort`): completions
of asynchronous transfers come back through it.

### The transfer (selector 1): its 0x1b0-byte structure (read)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | source surface id (`IOSurfaceGetID`) |
| 0x04 | 4 | destination surface id |
| 0x08 | 8 | completion callback; **0 = synchronous**: the call returns when the transfer is done |
| 0x10, 0x18 | 8 each | the callback's two reference values |
| 0x20 | 8 | option bits (below) |
| 0x28, 0x30, 0x38, 0x40 | 8 each | source rectangle x, y, width, height, **16.16 fixed point**; all zero = the whole source |
| 0x48, 0x4c | 4 each | the source surface's width and height |
| 0x50, 0x58 | 8 each | zero |
| 0x60, 0x64, 0x68, 0x6c | 4 each | destination rectangle x, y, width, height, whole pixels; all zero = the whole destination |
| 0x70, 0x74 | 4 each | the destination surface's width and height |
| 0x78, 0x80, 0x88 | 4, 8, 8 | source shared event (port), wait value, signal value |
| 0x90, 0x98, 0xa0 | 4, 8, 8 | destination shared event, wait, signal |
| 0xa8 | 4 | alpha override |
| 0xac .. 0xc8 | 4 each | border fill: x, y, width, height, alpha, then the Y/R, Cb/G, Cr/B values |
| 0xcc .. 0xd2 | 2 each | write-only mode values (alpha, Y/R, Cb/G, Cr/B) |
| 0xdc .. 0xec | 4 each | histogram: bin mode, x, y, width, height |
| 0xf0 | 8 x 4 | histogram pixel bins |
| 0x110 | 4 x 0x28 | up to four colour-processing blocks (`IOSurfaceAcceleratorCommApi`), each with a type word first |

The options dictionary becomes the option bits at 0x20 (each key's name as `IOSurfaceAccelerator.framework` exports it):

| Bits | Key | Meaning |
|---|---|---|
| 0-4 | `Transform` | bit 0 flip vertically, bit 1 flip horizontally, bit 2 turn 90 degrees; any value whose bits above 1 are not a single bit is refused by the library |
| 6 | `CustomFilter` | use the coefficients of selector 4 |
| 7 | `NearestFilter` | nearest neighbour |
| 8, 9, 10, 12 | `OptOutBlitClip`, `LockInScaler`, `SkipAsyncCallback`, `FixUpscaling` | |
| 13-14 | `DitherControl` | the value (default 1) |
| 15-17 | (none) | the source's chroma location, from its attachments, when no options are given |
| 18 | `AlphaOverride` | |
| 28 | the `BorderFill*` keys | fill a rectangle with a colour |
| 29 | `HistogramBinMode` | |
| 30 | `ZeroFill` | |
| 31, 32, 33 | `DisablePseudoLinearScaling`, `PixelAveraging`, `EdgeMirroring` | |
| 35 | `UnwireSurface` (value) | |
| 36-38 | `ColorRemapMode` (0 to 4) | |
| 39 .. 58 | `DirectionalScalingEnable`, `HDREnable`, `EnhanceEnable`, `NoiseEstimationEnable`, `ReadOnly`, `HtpcLargeHeaders`, `VraScaling`, `NoPowerEarlyBail`, `FirmwareTimeoutInject`, `ITUVariantCSCEnable`, the `WriteOnlyMode*` keys, `FilmGrainEnable`, `ReducedPrecisionFractionalOffsets`, `NoCoalesceAsyncIOFenceWrites`, `DestinationSurfaceDisplayBound`, `SymmetricScaling`, `DefaultChromaDownsample`, `BorderFillOddSizeYCbCrEnable`, `LegacyOddSizeYCbCrPixelExtension`, `BorderFillColorCompressedFormats` | one bit each, in that order |

### Who calls it, and how (read)

- **VideoToolbox** (`VTPixelRotationSession`, the hardware attempt of its three: the M2M scaler, its Metal session, software):
  `IOSurfaceAcceleratorTransformSurface(accelerator, source, destination, options, rectangles, NULL, NULL, NULL)`:
  **synchronous**. The rectangles are the source's x, y, width, height times 65536 (64 bits each) and the destination's in
  whole pixels (32 bits each). The rotation goes into `Transform`: flip vertical is bit 0, flip horizontal bit 1, 90
  degrees sets bit 2, 180 toggles bits 0 and 1, 270 toggles all three. Other options: `ForceMaxSpeed`, `ZeroFill`,
  `ReducedPrecisionFractionalOffsets`, the `Histogram*` keys. Before choosing the scaler it reads the service's
  `IOSurfaceAcceleratorCapabilitiesDict` (below) and refuses a transfer whose destination/source ratio is outside it; when the
  capabilities do not say `FractionalDimensions` it rounds the source rectangle out to whole pixels itself. 8-bit 4:2:0
  (`420v`, `420f`) needs no capability; 10-bit and other formats each have one.
- **CMCapture's `FigM2MController`** (`+queryCapabilities`, once per process) reads the same dictionary for
  `+maxUpscale` and friends. It needs **every one** of: `IOSurfaceAcceleratorFilterHorizontalTapsCount`,
  `...FilterVerticalTapsCount`, `...FilterHorizontalPhasesCount`, `...FilterVerticalPhasesCount`,
  `...FilterCoefficientsPrePointBits`, `...FilterCoefficientsPostPointBits` (integers), then
  `IOSurfaceAcceleratorCapabilitiesMaxHUpscale`, `...MaxVUpscale`, `...MaxHDownscale`, `...MaxVDownscale` (numbers read as
  CGFloat), `...SymmetricScaling`, `...MinSourceWidth`, `...MinSourceHeight`, `...MinDestWidth` (integers). It stops at the
  first that is missing and keeps zeros for the rest: that is how the S9 got a `maxUpscale` of 0x0.
- The camera preview's stitcher (`BWPreviewStitcherRenderer`, render mode 0) caps its up-scale at `+[FigM2MController
  maxUpscale]` and does its transfer through VideoToolbox's rotation session (CAMERA.md).

### The capability dictionary (read)

A registry property of the service, `IOSurfaceAcceleratorCapabilitiesDict`. Ratios are numbers of at least 1 for both
directions: VideoToolbox inverts the down-scale ones as it reads them. The keys iOS 27.2.1's readers look for:
the ones above, `...MaxSourceWidth/Height`, `...MaxDestWidth/Height`, `...MinDestHeight`, `...FractionalDimensions`,
`...PrecisionScalingFactor`, the formats `IOSurfaceAcceleratorFormatIn...`/`...FormatOut...` (`ARGB2101010`, `ARGB8101010`,
`LumaOnlyL008`, `LumaOnlyL016`, `2Planes42010`, `2Planes42210`, `2Planes44410`, `2PlanesPacked42010`, `...42210`, `...44410`,
`Interleaved422` in), `kSurfaceAcceleratorCapabilitiesMediaCompInput/Output`, `...InterchangeCompression`,
`...InterchangeLossyCompression`, `kSurfaceAcceleratorCapabilitiesColorManager`, `kSurfaceAcceleratorCapabilitiesHistogramValues`,
and others (HDR, film grain, LUT sizes) that only other paths read.

## The S9's service

### How the transfers are done, and why the CPU first

Three ways were weighed:

1. **The Exynos 9810's own scaler** (MSCL, the block the stock Samsung kernel drives as a V4L2 memory-to-memory device). It
   is the right end state (no CPU time, the hardware's own filters), but it needs a driver the port does not have: its
   power domain and clocks, its System MMU (it reads and writes through an IOMMU that has to be given each surface's pages),
   its registers and interrupts. That is a driver of its own and a phone bring-up of its own.
2. **The GPU** through the port's Vulkan (PanVK). The kernel cannot drive PanVK; the scaler would have to be a user-space
   daemon the kernel calls back into, for every transfer, with Metal's own transfer session already doing the same work
   in the same process when the M2M path is refused. Little to gain.
3. **The CPU, in the kernel, in the caller's thread.** Correct, testable on the build machine bit for bit, and its
   capabilities are exactly what the code does. Cost: CPU time in the calling process (measured below).

This is stage one: the CPU (`drivers/media/m2m/hakios.m2m_core.h`, plain C, a codec in `tools/arch-layers.conf`), behind the
same service and the same capability dictionary the hardware path will keep. Stage two, the MSCL, replaces the core's
`m2m_run` and may widen the capabilities; nothing above it changes.

### What it does

- **Formats**: Y'CbCr 4:2:0 in two planes, `420v` and `420f` (8 bits), `x420` and `xf20` (10 bits in 16-bit samples, high
  bits), any of them into any of them. Video and full range by H.273's equations; depths converted; no change of colour
  matrix (none is needed from Y'CbCr to Y'CbCr of the same matrix). RGB formats are refused (they would need the surfaces'
  colour matrix, which the port's IOSurface does not keep).
- **Geometry**: a source rectangle in 16.16 (fractional origin and size), scaled into a destination rectangle at up to 16
  times up and 16 times down on each axis, turned by 90 degrees clockwise and/or flipped (the flips after the turn, in the
  destination: inferred from VideoToolbox's mapping of 270 degrees to all three bits; a flip with a turn has not been seen
  on a phone).
- **Filter**: bilinear with 8-bit weights (256 phases) going up and going down by less than 2 (a bilinear tap already
  spans two source samples); going down by 2 or more on an axis, a box on that axis: the average of ceil(ratio) bilinear
  taps over each destination sample's footprint; `NearestFilter` honoured. The common case (one tap) steps the source
  position along each destination row and walks the destination in 32-sample tiles, so that a turned picture is read a
  cache-sized window at a time.
- **Chroma**: sited as 4:2:0 video is ("left": with the even luma column, half way between two luma rows), in both pictures;
  a turned or flipped destination's chroma is filtered at its own positions.
- **`ZeroFill`**: black (Y' black, Cb Cr at the centre) outside the destination rectangle; without it, untouched.
- **Synchronous only**. A transfer with a completion callback, shared events, a colour-processing block, the caller's own
  filter, border fill, HDR, enhancement, noise estimation, read-only, write-only mode, film grain or colour remapping is
  refused with kIOReturnUnsupported, so that the caller takes its next way (VideoToolbox: its Metal session). So are
  surfaces that are not two-plane 4:2:0 of those four formats, and surfaces the port keeps as snapshots of a process's
  memory. Selectors other than 1, 2, 3, 5 and 10 answer kIOReturnUnsupported.
- **Caches**: the source is cleaned and invalidated before it is read and the destination cleaned after it is written
  (`IOMemoryDescriptor::performOperation`), so a non-coherent writer or reader sees what the CPU saw.

### The capability dictionary it publishes

Made in `hakios.m2m_scaler.cpp` from `m2m_caps_get()`, whose numbers are the constants `m2m_check()` enforces
(`tools/m2m-scaler-test.sh` checks each one is the edge of what is accepted):

| Key | Value |
|---|---|
| `IOSurfaceAcceleratorFilterHorizontalTapsCount`, `...VerticalTapsCount` | 2 |
| `...FilterHorizontalPhasesCount`, `...VerticalPhasesCount` | 256 |
| `...FilterCoefficientsPrePointBits`, `...PostPointBits` | 1, 8 |
| `IOSurfaceAcceleratorCapabilitiesMaxHUpscale`, `...MaxVUpscale` | 16 |
| `...MaxHDownscale`, `...MaxVDownscale` | 16 |
| `...SymmetricScaling` | 0 |
| `...MinSourceWidth/Height`, `...MinDestWidth/Height` | 2 |
| `...MaxSourceWidth/Height`, `...MaxDestWidth/Height` | 8192 |
| `...FractionalDimensions` | 1 |
| `IOSurfaceAcceleratorFormatIn2Planes42010`, `...FormatOut2Planes42010` | 1 |

### Where it lives

- `drivers/media/m2m/hakios.m2m_core.h`: the pixels (plain C, no kernel).
- `drivers/media/m2m/hakios.m2m_scaler.cpp`: the IOService `AppleM2ScalerCSCDriver` and its user client
  `HakiosM2MScalerUserClient`; taken into the ios-27 tree as `iokit/Kernel/` by `profiles/ios-27.copy`. The fork of XNU
  lists the file and its personality (on IOResources) in `iokit/conf/files.arm64` and `iokit/KernelConfigTables.cpp`.
- Boot argument `m2m=0`: the service is not published (the phone is then as it was without it).
- The console: `hakios: m2m: AppleM2ScalerCSCDriver registered ...`, `client N opened (pid P)` (the first 32), each client's
  1st, 2nd, 30th, 300th and every 3000th transfer (and the service's 30th and 300th) with its surfaces, rectangles, transform,
  options and time (`pixels` the core, `caches` the cache maintenance), and the first 20 refusals with their reason.
- `tools/m2m-scaler-test.sh` (CI): the core against reference pictures computed a second way in double precision (at most
  one code apart, positions within 2^-20 pixel), whole-picture turns and flips as exact permutations, four quarter turns as
  the picture again (luma; chroma is filtered by its siting), flat colours through every range and depth, and each published
  limit as the edge of what `m2m_check` accepts.

## The phone test

Phone 1 (2ab4dd7d2a037ece), kernel L6bt's tree (scion-kernel `b763d67b` + this branch, the fork's `c2f63b275` + the two
lines above), the boot line of L6btm, the root the phone had (bootline-1124's hand-back image, with PR 412's `cam.dylib`),
with `cam_trace=1`, and `cam_metal=1` where said. Consoles on the build Mac (`scion-phone1/logs/con-xnu-boot-M*-1124-*.log`),
notes in `agents/m2m-scaler-1124/NOTES.md`. All **measured**:

| Boot | Line | What it showed |
|---|---|---|
| M1 | `cam_trace=1` | `AppleM2ScalerCSCDriver registered` at boot; clients opened by backboardd and chronod; chronod's transfers `'xf20'` 56x56 to 128x64, 0.4 ms each. Camera: the source lit, but **no preview pipeline at all** (`Preview(PrimaryScaler) leads nowhere`): without `cam_metal=1` cameracaptured has no Metal device and builds no preview (camera_metal.c) |
| M1off | `cam_trace=1 m2m=0` | the control: `m2m=0: no memory-to-memory scaler published`; the same missing preview. So that was the line, not the scaler |
| M2 | `cam_trace=1 cam_metal=1` | `trace: stitch: IOSurfaceAcceleratorCreate: 0 (0), accelerator made`; the stitcher's transfer 1 from **rect 376,282 752x564** (the 2x crop; it was 753,565 0x0 on 23 November), transfer 30 from 0,0 1504x1128, both to 0,178 828x1106 of 828x1702; `hakios: m2m: transfer 300 (pid 55)`: cameracaptured's transfer done by this service, transform 4 (the quarter turn), **107 ms** (the first core: four taps at 1.36x down, a call per sample, the caches of both surfaces) |
| M3 | `cam_trace=1 cam_metal=1` | the faster core: cameracaptured's transfers 1504x1128 to 828x1106 turned in **14.8 to 15.4 ms** (13.8 to 14.3 ms the pixels, 0.6 to 1.2 ms the caches; one of 34 ms). Back camera (on the desk): source Y 13, stitcher output Y 8, image queue sink Y 8. Front camera (lit room): the stitcher's transfers with **transform 6** (the turn and the horizontal flip: the front camera's mirror) and the front's zoom crop 176,132 1152x864; source Y 112, **stitcher output Y 70, 160 of 256 grid points above black** (the picture fills 1106 of the 1702 rows), the same at every node after it and at **the image queue sink**. The home screen and its widgets (chronod's transfers) look as before |

**The screen**: in M2 and M3 the viewfinder is still **black** in the screenshots (back and front), although the image
queue sink's surfaces hold the lit picture. backboardd makes a `fmt 500` texture (YCbCr8_420_2P) over the sink's 828x1702
surface when the preview starts (and `fmt 550` after the flip); the picture is lost there, on the display side, between the
image queue and the screen. That is the next cause to find; the 23 November experiment (the stitcher fed by VideoToolbox's
Metal path) ended at the same place (CAMERA.md).

**The photograph**: the shutter gives AVFoundation error -11800 with -16808 underneath at once, and "StillImage: no pool of
the graph's in the configuration", as before this service: it changes nothing there.

**Heat**: with `cam_metal=1` cameracaptured runs at 100 % from boot (its Metal shaders; 23 November saw the same with a warm
cache), and the big cores reach 69 to 82 C in a camera window (M2 passed 78 C and was stopped; M3 stayed under).

## Not done

- **The viewfinder on the screen**: the display side of the preview (backboardd's 4:2:0 texture over the image queue's
  surfaces), above. Not the scaler's.
- **The Exynos 9810's MSCL** as the transfer engine (stage two): its power domain, clocks, System MMU mapping of each
  surface, registers and interrupt; behind the same service and dictionary. Until then the CPU costs about 15 ms of the
  calling thread a preview frame (cameracaptured).
- **Asynchronous transfers** (a completion callback, delivered through the notification port), shared events, histograms,
  border fill, the caller's own filter coefficients (selector 4), captures (selector 0): refused today, so their callers
  take their other way.
- **Other formats**: RGB (needs the surfaces' colour matrix, which the port's IOSurface does not keep), 4:2:2, 4:4:4,
  packed 10-bit, compressed (`MediaComp`, interchange) formats. Each would add its capability key when it is done.
- **Flip and turn together** is taken as "turn, then flip in the destination" (inferred from VideoToolbox's numbers for 180
  and 270 degrees, where the order does not matter); the front camera's preview uses transform 6, whose picture could not be
  looked at on the screen yet.
- The log: chronod opens a client for each widget picture and each client's first transfers are said, so a boot prints
  about 140 `transfer` lines in a burst (the `client N opened` lines stop at 32). To be trimmed.
