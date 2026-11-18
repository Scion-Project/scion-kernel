# Metal on iOS 27 for the Galaxy S9: Starbite's Metal stack rebuilt, and what ran (8 and 9 November 2026)

iOS 27 on the S9 renders with OpenGL ES through `sbca` ([GPU-PATH.md](GPU-PATH.md)). This page is about the other route: Apple's Metal
framework on the Mali-G72, through a Metal plug-in that Starbite wrote for iOS 26 (a Metal 2 frontend, `HotMetal2.mm` and
`NativeBridgeMetalFrontend.mm`; metal2vulkan's bridge; Mesa's PanVK over the kernel's StarbiteMali). The scripts and tools in
`tools/ios27/metal/` rebuild that stack for iOS 27.0.1 and test it. Starbite's own part of it is in `tools/ios27/metal2/` (the two frontend files,
`HotMetal2.mm` and `NativeBridgeMetalFrontend.mm`, `build-m2v-bridge.sh`, the bridge's Rust sources and its patches 0001-0016 and 0100-0103,
`Info.plist`, `m2vtest.c`; MIT, credited below), so the scripts and the three patches in `tools/ios27/metal/patches/` work from this repository
as it is. No binary of any kind is in the repository.

## What is here (`tools/ios27/metal/`)

| File | What |
|---|---|
| `build-host.sh` | SPIRV-Headers, SPIRV-Tools (Vulkan SDK 1.4.363.0) and SPIRV-LLVM-Translator v23.1.3, against Homebrew's llvm 23: the host tools Mesa 26's `mesa_clc` needs |
| `mk-panvk27.sh` | `tools/ios27/mesa-ios/mk-panvk26-ios.sh` with its paths from the environment: the host tools (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile`), then PanVK of Mesa 26.2.6 for iOS arm64. It unpacks the release tarball (sha256 checked) and applies `tools/ios27/mesa-ios/`'s `mesa26-panvk-ios`, `-panvk-null-vs`, `-panvk-shader-size-cap`, `-bifrost-ra-multispill`, `-panvk-host-images`, `-panvk-tiler-heap-halves` and `-panvk-batch-stats` patches in that order, and copies the repository's `starbite-drm` shim in at every run; `HOST_TOOLS=` reuses built host tools. The PanVK of phone 1's c2 bundle (`cc241e55`, 14 November) was built from a local tree; the last three patches record it, and a build from them has the same exports and imports and differs only in the shim (one fence implementation, see below) and the two braces `-panvk-batch-stats` restores. |
| `rust.sh`, `bridge.sh` | Rust 1.98.0 with the iOS target, then metal2vulkan's bridge (`libnative_metal_bridge.a`) through `tools/ios27/metal2/build-m2v-bridge.sh` (it clones metal2vulkan at 720ef1ce and applies the bridge patches) |
| `mk-bundle27.sh` | `StarbiteMetal.bundle` (frontend, bridge, PanVK; arm64, ad hoc signed) and `m2vtest`; the frontend and `HotMetal2.mm` are built from a copy with `frontend-dexr.diff` and `hotmetal2-diag.diff` applied (`NOPATCH=1`: as they are in `tools/ios27/metal2`) |
| `mtl27test.m` | the smoke test, six steps: the kernel's services (StarbiteMali, `IOAcceleratorES`, `IOCoreSurfaceRoot`), IOSurface, `MTLCreateSystemDefaultDevice()` through Metal's own discovery (or `--direct`), a render-pass clear read back, a render pass into an IOSurface-backed texture (what CoreAnimation does), and QuartzCore's compute and render shaders into pipelines |
| `sbmtlc.c`, `sbwall.m` | `sbmtlc.dylib`, a library for `sweetinject`: any inserted library makes the kernel start the program with `ptrauth_disabled=1` (xnu `0324`), which is what lets dyld take the arm64 plug-in into an arm64e process; it also captures `os_log` (logd is off on this root: an in-place hook of libsystem_trace's four `_os_log_*impl` entry points, opt-in by `/private/var/local/oslog-clients`) and `sbwall` draws a static wallpaper in SpringBoard from `/private/var/local/sbwallpaper.png` (opt-in: `install-metal27.py --wallpaper FILE` takes the image from the builder's own root, or any file on the Mac, at install time; no image is shipped here) |
| `install-metal27.py` | puts the stack into an attached iOS 27 root (`STAGE=<dir> sudo python3 install-metal27.py /Volumes/ROOT [--clients "mtl27test PosterBoard CollectionsPoster"] [--oslog "PosterBoard"] [--wallpaper FILE] [--no-job] [--uninstall]`), keeps every file it replaces as `NAME.before-metal27`, adds two run-once launchd jobs (`com.starbite.m2vtest`, `com.starbite.mtl27test`) |
| `mtl-cmdline.py` | a command line plus `sbmtl=1` (`--posters`: `sbmtlc` into PosterBoard and CollectionsPoster; `--metal-ca`: `sbgl=0`), staying under the 608-byte limit |
| `patches/` | `frontend-dexr.diff` (`SWEET_MTL2_DEXR=1`, see below, and a log line for each new texture shape), `hotmetal2-diag.diff` (an "opening device" log line), `starbite-drm-diag.diff` (a console line on the StarbiteMali connection), `frontend-dexr-surfaces.diff` (with `SWEET_MTL2_DEXR=1`, a pipeline drawn into an extended-range texture over a surface is made again in that format: the app switcher's background, [RENDERING.md](RENDERING.md)), `frontend-nocopy-diag.diff` (a line on the memory of each of the first 16 refused no-copy buffers), `frontend-nocopy-readonly.diff` (a no-copy buffer over memory the process may never write, which the kernel cannot wire, is made over a copy of its bytes: the app switcher's cards, [RENDERING.md](RENDERING.md); `SWEET_MTL2_NOCOPY_RO=0` refuses it as before) |

Licences (`LICENSES.md`): the files here are Scion's, MIT. The frontend and the bridge are Starbite's, MIT (they come from IOS-26-Hot and, before it, UniXNU's
AdrenoMetal, credited in `NOTICE`); metal2vulkan (steelbrain) is GPL-2.0-or-later OR LGPL-3.0-or-later and is linked statically into the bundle (ship its source with any
binary); Mesa is MIT.

## The kernel needs nothing new

StarbiteMali (`0301`, `0305`, and the Mali fixes of the `03xx` range: among them no write-combine for imported and client memory) and StarbiteAccelerator (`0336`; class `IOAcceleratorES`,
`MetalPluginName` StarbiteMetal, device class `HotMTL2Device`, boot-argument `sbmtl=1`) are in the iOS 27 series. iOS 27's Metal still matches
`IOAcceleratorES` plus `MetalPluginName`/`MetalPluginClassName` and loads the plug-in from `/System/Library/Extensions` (checked in the 24A446 cache: the strings
are in Metal and IOGPU, `_MTLDevice` is exported).

## Install, test, switch off

1. Build the stack (above), then install it into an attached root with `install-metal27.py`; give the command line `sbmtl=1` (`mtl-cmdline.py`).
2. On the USB log: `STARBITE MTL: IOAcceleratorES published` (kernel), then `MTL2[pid mtl27test]: plugin loaded`, `native-metal: _MTLDevice is N bytes`
   (or `MISSING <class> -<selector>`), `MTL27TEST: 1 kernel .. 6 shaders PASS|FAIL`, `RESULT PASS` (also `/private/var/tmp/mtl27test.log`); `/private/var/tmp/m2vtest.log`
   has `step 1 device` to `step 5 import` (step 6, QuartzCore, is skipped on purpose).
3. Failure modes: step 3 gives nil and no "plugin loaded": Metal never loaded the bundle (check `sbmtl=1`, the path, the signature); "plugin loaded" and
   "device refused": PanVK or StarbiteMali (`mtl2.log`, `m2vtest.log`).
4. Off: drop `sbmtl=1` (Metal finds no device again), or empty `/private/var/local/mtl2-clients`, or `install-metal27.py --uninstall`.

## What ran

On a Galaxy S9 (8 November, the iOS-target kernel, a root that is the builder's own): `mtl27test` steps 1 to 5 passed: `MTLCreateSystemDefaultDevice()`
returned "Mali-G72 MC18", the clear read back exact, the IOSurface render exact. `m2vtest` (the bridge straight on PanVK): compute, render and import, no bad pixel. Step 6
failed from the test's own shader pairing, since fixed. PosterBoard gets a Metal device (20 s to open). The wallpaper is still black: PosterBoard's data
store is empty and no poster extension is ever started (although LaunchServices lists the `com.apple.posterkit.provider` extensions), so `sbwall` is a stop-gap.

On 9 November, with `sbgl=0` and backboardd in `mtl2-clients` (CoreAnimation's own Metal renderer in backboardd), CoreAnimation did run on Metal: `MTL2[backboardd]: device
Mali-G72 MC18 in 4183 ms`, about 4.5 thousand pipelines cached in `m2v-cache` (the first boot spent about a minute of CPU on shader compiles). It also failed: 18 command
buffers were refused at `variable_blur_clamp_downsample_frag_lph` (sampling an XR10 texture through a mip-filtered sampler is `NotImplemented(Format)` in metal2vulkan) and 12 no-copy
buffers were `NotRepresentable`. `patches/frontend-dexr.diff` is the answer tried for the first: with `SWEET_MTL2_DEXR=1` (in `mtl2-env`, off by default) the extended-range
formats (BGRA10_XR 552/553, BGR10_XR 554/555) of textures that live in device memory only are held as RGBA16Float, with the pipelines' attachments to match. **Not run** when this
was written: that patch on a phone, and the frame rate of CoreAnimation on Metal against `sbca`'s OpenGL ES path.

What came of the frame rate question is in [LIQUID-GLASS.md](LIQUID-GLASS.md): where the Metal path lost its time, what was changed, and the runs.

## Not done

- The wallpaper (above), and the 12 `NotRepresentable` no-copy buffers.
- The decmpfs type 16 of nine SplashBoard `*.ktx` snapshots (the kernel registers type 1 only): unrelated to Metal.
- The scripts were run against Starbite's tree (`SB=`) before `tools/ios27/metal2/` was imported; the import changed their paths only (`tools/metal2` to `tools/ios27/metal2`, the repository as the tree). `bridge.sh` and `mk-bundle27.sh` were not re-run on a phone for it.

## The bridge library the bundles link (recorded 18 Nov 2026)

Every Metal bundle since 12 Nov links `libnative_metal_bridge.a` as built on the Mac mini on 12 Nov 17:45 (metal-perf). The tree
it was built from had changes that were not in this repository: the render-pass-instance split counters in metal2vulkan's
runtime (`INSTANCE_SPLITS` in `lib.rs`, counted in `render.rs` where a draw cannot record in the current instance) and, in the
bridge, the kept runtime messages and the work-counts text (`nmb_take_message`, `nmb_device_work_counts_text`) that the
plug-in's telemetry (`touch /var/local/mtl2-telemetry`) prints with `new_instance_<reason>=N`. They are now
`tools/ios27/metal2/bridge/patches/0103-sweet-instance-split-counters.patch` and
`tools/ios27/metal2/bridge/NativeMetalTelemetry.rs` (include!d by `NativeMetalObjects.rs`; the code is unchanged, only moved
to a file of its own for the size rule) with the two declarations in `NativeMetalObjects.h`. Checked: metal2vulkan 720ef1ce
plus patches 0001-0016 and 0100-0103 gives the 12 Nov tree's metal2vulkan sources file for file. The library was not rebuilt
from the repository for this (a 300 MB Rust build); the counters cost one relaxed atomic add per new instance.

## The bridge's CPU cost on the Mac (`tools/ios27/metal/host-bench`, 18 Nov 2026)

The phone shows a frame's encode time but no profile of it inside the bridge (the kernel's profiler prints only the hottest
program counters, which leaves out code as spread out as the runtime's). `tools/ios27/metal/host-bench/build.sh` builds
Mesa's KosmicKrisp (Vulkan over Metal) for the Mac from the same Mesa tree, a shim that lets metal2vulkan open it as its
loader, and `bench`: the bridge's C API (the host build of `build-m2v-bridge.sh`) driving a pass of CoreAnimation's main pass
shape (828x1702 BGRA8 loaded and stored, RGBA16F and R32F memoryless; N draws with vertex bytes, a vertex buffer, a scissor,
optionally a texture and fragment bytes, a pipeline change every K draws), one frame at a time. `sample <pid>` gives the
profile. The driver's own cost is KosmicKrisp's, not PanVK's; the runtime's is the phone's, at the M1's speed.

First numbers (M1, 100 draws, 16 pipelines changed every draw): 3.5 us a draw; 7.1 us with a texture bound a draw. In a
profile of the textured case the per-draw path is spread over the runtime (`draw_resident`, `settle_draw`, `Recording::record`,
`instantiate_into`, `Bound::dynamic`, none above about 10 %); at the pass's end `clear_reach::DrawnArea::tiles` took about two
thirds of `end_pass` for scissors that differ every draw (it marks the bin grid one tile at a time; a draw whose scissor
covers the whole pass skips it, as most of CoreAnimation's main-pass draws should). The M1 runs this single-threaded code
several times faster than the S9's M3 at 1794 MHz.

## Shader compiles: off the UI, bounded, shared and warm (19 Nov 2026)

What an iOS 27 boot on the S9 showed (phone 2, `con-xnu-boot-L4-1118-2103.log`): every Settings open compiled the same 24 522-NIR compute
shader (250 224 bytes) for 24 to 28 s on one core with the GPU idle and the UI stalled; Preferences then began a fragment shader of 3 023
NIR instructions that never ended (Preferences at 130 %, launchd at 40 %, for more than three minutes). The causes and what changed:

| Cause | Change |
|---|---|
| PanVK compiled on the thread that asked for the pipeline, and the completion-handler `new...PipelineState` calls ran the compile on the caller too | `patches/frontend-async-compile.diff`: a compile runs on a worker (three at a time, QOS utility); the synchronous calls wait for it at most `STARBITE_M2V_COMPILE_TIMEOUT_MS` (default 5000, 0 = for ever) and then fail like a refused pipeline, the compile going on in the background and its result waiting for the next request of the same pipeline (key: the functions' bit-code hashes and the state; while one is still running a repeat waits 100 ms, and with four such compiles running, 200 ms); the completion-handler calls return at once. A pipeline whose compile failed after more than 10 s is not tried again for 10 minutes |
| A compile with no end: Bifrost's register allocator spills a node (or, past four rounds, up to 32) per round and rebuilds the interference graph each time | `mesa26-panvk-compile-budget.patch`: `STARBITE_PANVK_COMPILE_BUDGET_MS` (default 90 000, 0 = none) is a wall-clock budget of one compile; the allocator's rounds, its LCRA solve and its liveness loop check it and leave the compile by `longjmp`; the pipeline fails (`PANVK: ... abandoned after N ms (compile budget)` on the console) |
| One cache directory per program (`/var/local/m2v-cache/<progname>`), so only backboardd's was ever filled, a sandboxed program fell back to its own caches directory, and Preferences or SpringBoard never saw what another had compiled | `patches/hotmetal2-shared-cache.diff`: one directory per user, `/var/local/m2v-cache/u<uid>` (the runtime's files are owner-only), for every process; `STARBITE_M2V_CACHE=/dir` (mtl2-env) names another |
| The directory was made by the first process to run, with nobody to ship anything in it | `steps.d/81-m2v-cache.sh` makes `/var/local/m2v-cache` (0777), removes the per-program directories, and installs a warm cache (below) |

**A warm cache.** The runtime's entries are named by content: translated functions and pipelines under `v1/<build>/` (the build is the
bridge library's), PanVK's pipeline data in the content directory beside them (named by the SPIR-V, the pipeline state and PanVK's cache
UUID, `driver.rs`). PanVK itself cannot compile on the Mac for the Mali-G72 (it needs the kernel's GPU), so a warm cache is harvested from a
phone: boot the bundle that will ship, open Settings, Display, Appearance (Liquid Glass), the app switcher and Control Center until the
`PANVK: ... NIR instructions` lines stop, copy `/private/var/local/m2v-cache` off, run `tools/ios27/metal2/harvest-m2v-cache.sh PULLED`
(it keeps the newest build directory and the content directory of each `u<uid>/`, into `tools/ios27/fullroot/warm-m2v`, which is not in git),
and build the root: step 81 installs it, owner-only, owned by the uid. It holds for that bundle and that PanVK (about 33 MB); another
build finds none of it and compiles as before.

**Format 500 is `MTLPixelFormatYCBCR8_420_2P`** (metal2vulkan's `generated.rs`), not an unknown format: the runtime has it (`pixel_format_info.rs`,
`bind/ycbcr_convert`, converted in the shader), and the frontend's texture over an IOSurface passes the surface's YCbCr matrix on. The
`texture memory fmt 500` line is that path (`how` is "memory" for textures over an IOSurface or a buffer: they have no device upload, by
design, so no `texture device` line follows), and nothing was refused. Why the reference image did not draw is not in that log: the pipelines that sample it may simply not have been made yet (the compute and
fragment compiles above were still running), which the two changes above address; a phone run of the new bundle will say.
