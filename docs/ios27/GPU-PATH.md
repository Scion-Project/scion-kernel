# iOS 27 on the Galaxy S9: what the GPU track found (8 November 2026)

Notes from the run that took the S9's home screen from CPU compositing (about 0.1 to 0.7 frames a second) to CoreAnimation rendering on
the Mali-G72 (10 to 51 frames a second while it is used). The code is in the tree: `userland/src/sbca.c` (the renderer hook, with iOS 27.0.1's
QuartzCore addresses, numbers only), `tools/ios27/ca-gpu/` (the build and the EAGL bridge), and in the kernel `SweetFramebuffer.cpp` (the GPU
present, the release notifications) and `SweetSurface.cpp` (the IOSurface copy fallback) under `xnu/files-12377/iokit/Kernel/arm/`.
Everything below was measured on one phone; none of it is a benchmark.

## What decided the frame rate

- **The frame pacer was an unanswered request.** `CA::EDRClient::client()` asks backboardd's own libEDR server
  (`com.apple.libEDR.Notification`) for a client with `EDRCreateClientForDisplayWithTimeout`. On the S9 the server never answers, every attempt
  times out after about 5 seconds, and every frame waited for it: one swap per 5.2 s. `sbca` makes that function return 0 (no EDR client, as when
  libEDR is absent); the environment variable `SBCA_EDR=1` keeps the original. That change alone is what took the phone from 0.1-0.7 to 10-51
  frames a second. The earlier kernel-side attempt (`sweetfb_relbuf=1`: publish `SupportRelbufInfoCb` and send IOMFB's release notification,
  type 7, for every presented or replaced swap) did not change the rate by itself; it is harmless and off by default.
- **The panel copy.** The CPU's scaled copy of a frame to the 828x1792 panel took 48.8 ms; the Mali GPU present (`sweetfb_gpu=1`) takes 2.0-2.3 ms.
- **CoreAnimation on the GPU.** `sbca` replaces CoreAnimation's software renderer with its OpenGL ES one, over Mesa's Panfrost behind HakiOS's EAGL
  bridge (136 texture functions in iOS 27's table). Home-screen icons render (they were grey placeholders), so does the Control Center blur. A
  CoreAnimation Metal renderer in backboardd was tried with the Metal stack: its plugin loads but no device is created, so it falls back to GL.

## The iOS-target kernel and wired memory

On the iOS-target kernel (`tools/ios27/build-xnu-12377-ios.sh`) backboardd's image memory could not be wired: `prepare()` failed with
`KERN_PROTECTION_FAILURE` read-write and `kIOReturnNoResources` read-only, which gave thousands of "cannot wire" lines and failed client-memory
IOSurface creates (the Settings icons). The fallback in `SweetSurface.cpp`: when the caller is the task itself, copy the pages into a kernel
buffer (`copyin`) and make the surface from that. Images do not change after they are wrapped; the surface simply no longer aliases the
task's pages. The log line is rate-limited.

## Settings is killed after 5 seconds

RunningBoard: "Failed to terminate gracefully after 5.0s". The main thread was stuck in
`-[PSSystemConfigurationDynamicStoreEthernetWatcher ethernetNetworkInterfaces]` (a configd request that never completes). `sbca` is injected
into Preferences too (`sweetinject=/usr/lib/sbca.dylib,backboardd,Preferences`) and replaces that method with one that returns an empty array.

## Root edits that went with it (on the builder's own root)

- `/usr/lib/sbca.dylib` and, beside it, libsbgl and libOSMesa (`tools/ios27/ca-gpu/build-sbca27.sh`).
- `CAEnableIOSurfaceCompression` and `CAEnableFramebufferCompression` set to NO in mobile's `.GlobalPreferences`.
- Jobs out of `launchd.plist`: `tools/ios27/starlte-disabled-ios27.list` (the ones this run measured: axassetsd, logd, sharingd, fileindexerd,
  modelcatalogd, modelmanagerd, beside `PerfPowerServices` of the base list). `tools/ios27/starlte-disable-jobs.py` keeps them in
  `launchd-disabled.plist`.
- The 24 HEIC layers of `/Library/Wallpaper` rewritten as PNG data (the originals kept in /private/var/local/wallpaper-heic-backup). The wallpaper is still black: PosterBoard has
  no poster configuration, and no poster extension is launched.

## Not solved

- The wallpaper (above), and Metal in backboardd.
- The boot-argument line is limited to 607 bytes: a second injected library needs a short path
  (`sweetinject=A,backboardd,Preferences;B,PosterBoard`).
- Frame pacing beyond the EDR fix.
