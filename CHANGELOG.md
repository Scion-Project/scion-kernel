# Changelog

What changed, newest first. One line a change: the part of the system, then what is different for whoever runs it.
The commits say how; `docs/` says why.

## Sources

What a build is made of, and where each part is fetched from. Nothing of these is kept in the repositories but the
changes to it; the scripts named fetch the release and put the changes on.

- the kernel: Apple's [XNU](https://github.com/apple-oss-distributions/xnu), tag `xnu-8796.121.2` (the release the Darwin 16.5 user space was built from) -- `xnu/apply.sh`, the changes in `xnu/hakios.patch` and `xnu/files/`, the drivers in `drivers/`
- the data volume: Apple's [HFS Plus](https://github.com/apple-oss-distributions/hfs), tag `hfs-627.100.6` -- `xnu/hfs/hfs.sh`, the changes in `xnu/hfs/hfs.patch`
- the boot: [U-Boot](https://source.denx.de/u-boot/u-boot) `v2026.07` -- `hakios-uefi/u-boot/apply.sh`, the changes in `hakios-uefi/u-boot/hakios.patch` and `files/`; the device's part in `hakios-sm7325/u-boot/`
- the UEFI (the other way to start): [Project Silicium's Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) at `e51cd877e31c` -- `hakios-uefi/apply.sh`
- the GPU's OpenGL ES: [Mesa](https://archive.mesa3d.org/) 25.0.7, the freedreno driver for the Adreno -- `tools/mk-mesa.sh`, the changes in `tools/mesa/`
- the GPU's Vulkan: the same Mesa's Turnip -- `tools/mk-turnip.sh`, the changes in `tools/mesa/turnip-darwin.patch` and `tools/mesa/hakios-drm/`
- Metal, the direct path: `hakios-metal/` in this tree -- `userland/build.sh METAL_PLUGIN=ca`, with `userland/src/HakiosMetalCA.m`
- Metal, on Vulkan: [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) at `243d63c` and [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) at `624bab3` with AdrenoMetal's 30 patches -- `tools/mk-m2v.sh`, the changes in `tools/m2v/`
- the build tools: Apple's [libdispatch](https://github.com/apple-oss-distributions/libdispatch), [libpthread](https://github.com/apple-oss-distributions/libpthread) and [AvailabilityVersions](https://github.com/apple-oss-distributions/AvailabilityVersions), [bootstrap_cmds](https://github.com/markmentovai/bootstrap_cmds), [unifdef](https://github.com/fanf2/unifdef), the [macOS 13.3 SDK](https://github.com/joseluisq/macosx-sdks) and the [iPhoneOS 16.5 SDK](https://github.com/theos/sdks) -- `xnu-deps/setup.sh`
- not fetched by anything here: the Darwin 16.5 user space, which you lay out from your own IPSW (`sysimage/`), and the phone's own firmware, pulled from its Android (`fw-*`)

## 2026-11-08

- iOS 27 on the Galaxy S9: the `ios-27` profile (machine `STARLTE`) builds a kernel that booted iOS 27.0.1 to SpringBoard on a phone: `patches/xnu-12377/series.ios27` (Project Starbite and the layers under it, wardevsa's S7 layer with his permission, Scion's `0400` and up), the files it adds in `xnu/files-12377/`, `xnuloader/sboot/` (the S-Boot loader), `tools/ios27/`, `userland/src/sbca.c` (CoreAnimation through OpenGL ES on the Mali-G72). Default `cpus=8`. `make kernel` does not build it yet. `docs/ios27-starlte.md`, `LICENSES.md`
- HakiOS (oosexgang/hakios-xnu-kernel) merged up to its `63fc2b46`: 276 commits, below under their own dates. What stays Scion's: `VERSION`, the Galaxy S9's framebuffer paths, panfrost beside msm in `tools/mesa/hakios-drm`, the S9's SpringBoard defaults. What is new here and off for the S9: `fb_beam` starts no look on a SoC with no known display interface, `dl_max` (display-link rates) is opt-in, `wallpaper_freeze=1|2` and the Metal arguments are as upstream's, off unless given
- `hakios-metal/` is in this tree now (MIT, its own LICENSE and NOTICE; the repository beside it has moved here upstream), with `userland/build.sh METAL_PLUGIN=ca`, `TURNIP=1` and `M2V=1`, `tools/mk-turnip.sh`, `tools/mk-m2v.sh`, `tools/ca-metallib.py` and the AIR front end of `tools/mesa/air`. Nothing of it is in the S9's configuration. `userland/src/vk_test.c` stays the gfxstream test; upstream's Turnip test is `vk_turnip_test.c` (`rc.jobs.vkturnip`). HakiosGPU's userptr request is 0x7d as upstream has it, so Scion's GPU-time request is 0x7c now
- ios-26 base: three small fixes from UniXNU's series, credited (`patches/xnu-12377/0096` the PAR_EL1 ISB, `0097` the 64-bit physical address in the pmap's page-data sync, `0098` arm64 processes with the PAC keys off, only with `SCION_NATIVE_PAC=1`). Their notice: `patches/xnu-12377/LICENSE.UniXNU`
- licences to settle before any distribution of a Metal-on-Vulkan binary (`M2V=1`), and for `userland/src/HakiosMetalCA.m`, whose origin states none: `THIRD_PARTY_LICENSES.md`, `COMPLIANCE.md`

## 2026-11-05

- Galaxy S9: the front camera's stream (`cam_s9=7`) starts when something reads `/dev/hakios-camera` or a camshot is asked for, and the camera is powered down again at the last close or 2 s after the last read; it was streaming at 30 frames a second from boot to power off with nobody using it. `cam_s9_always=1` keeps the old behaviour
- repositories: renamed, and the old URLs redirect: `scion-kernel` (this one), `scion-starlte`, `scion-star2lte`, `scion-crownlte`, `scion-installer`; names in the code stay HakiOS's
- README: starts with what a phone's owner needs; issue templates for a bug and a tester's report

## 2026-11-04

- S9 defaults: hardware video on (`mfc=5 mfc_irq=1`, 128 MB of RAM kept for it): Safari's video seen smooth on the phone; zero copy stays off. The real key bag stays off: on a real /var a passcode set from Settings didn't take (`keystore_real=1`, 4 November).
- Galaxy S9, with `tee_probe=3` or `4` (off by default, not yet run on a phone): Kinibi asked over MCP, a harmless TA opened and closed, then Samsung's keymaster TA for one test, an HMAC key's blob in `/private/var`. The key store does not use it yet.
- Galaxy S9, with `mfc=5` (still off by default): a video the block can't take (10-bit, or no room) goes to software; a seek works; every test frame now matches ffmpeg, after a fix for frames overwritten before the client had them. Safari, HLS and Photos tested. HEIC doesn't use the block.
- Key store: `keystore_real=1` (off by default) wraps the keychain's keys under Argon2id of the passcode, and locking drops classes A, B and "when passcode set": a forgotten passcode loses them, `passcode_reset=1` destroys them, turning it on drops today's keychain. Files are still not encrypted. Per-selector entitlement checks only log unless `keystore_entcheck=2`, which today's phones must not set. Without the argument nothing changes. Seen in QEMU and on a Galaxy S9 with `/private/var` in memory (unlock about 0.35 s); class A while locked is unverified on a phone. See `docs/keystore.md`.
- Kernel: AES, SHA-1 and SHA-256 by the ARMv8 crypto instructions, where the processor has them (`mc_ce=0`: the C), checked by the same known answers first. Galaxy S9: SHA-256 944 MB/s (C 130), SHA-1 944 (C 127), AES-128-CBC 311 (C 62); `sysctl kern.hakios.crypto_bench` measures both
- Galaxy S9: hardware H.264 and HEVC decoding on the Exynos MFC, off by default (`mfc=5`). 1080p30 through AVPlayer: `mediaserverd` 17% of a core, 63% in software. Firmware from the phone's own VENDOR partition; with `mfc=`, 128 MB of the low bank stay out of XNU's RAM; not yet seen in Safari or Photos
- Galaxy S9, S9+, Note9: at 1125 x 2313 the passcode keypad sits 41 points higher, so Emergency and Cancel are clear of the home indicator, and the Phone app's keypad and the emergency dialer have the iPhone X's layout (they were jammed together; `libhakios.sb.dylib`, now also in MobilePhone and InCallService). Seen on an S9.
- Galaxy S9, S9+, Note9: the USB console comes back when the cable is unplugged and plugged in again, or when the host resets the port, with no restart of the phone: the port's VBUS and the USB-C switch are watched, and the link restarts from the PHY up, as Android's does on every attach. New console commands: `usb`, `usb restart`, `usb drop MS`, `usb unplug MS`, `usb muic MS`, `usb vbus MS`, `usb watch on|off`. Seen with restarts and drops made from the phone and the host's re-enumeration; a real cable replug has not been tried
- Galaxy S9, S9+, Note9: backboardd no longer dies now and then while the screen is being drawn, which restarted SpringBoard and every app with it (a bad write in Mesa's panfrost: each framebuffer preload wrote its padding descriptor 0x100 bytes on instead of 16, and crashed when that crossed the end of a descriptor pool: `tools/mesa/panfrost-cast-and-pack.patch`)
- Galaxy S9: `tee_probe=N` (off unless asked, in no default line) asks the secure OS whether it is there (1: the info fastcall on core 0, read only) and starts it as far as its own state (2: the init call, the init values, an N-SIQ, the state polled, 30 s at most). On the phone it answers (MCI version 1.6) and reaches state 2, initialised, and stops there. No TA, daemon, storage, RPMB or EFS
- Galaxy S9, S9+, Note9: no more tearing from `fb_zc`: SwapWait now answers once the swap is on the panel (`fb_swapwait=1`, on by default with `fb_zc`; `fb_swapwait=0` is how it was), so CoreAnimation no longer draws into the surface the display controller is sending to the panel. Measured on a test build; nobody has judged it by eye yet. Refreshes missed in 11 s of swiping: 27, 28, 34, against 24, 73, 84 before; an earlier run, on 3 November, had 35 in 367 frames with it against 17 to 24 in 390 without.
- Galaxy S9: the status bar's Wi-Fi bars follow the signal: while a network is joined the kernel measures the link every 5 s and posts Apple80211's RSSI event, the only thing wifid scales the bars from. It never did, and the RSSI it reported was the last scan's before the join (the POCO's station does not measure yet: `link_quality` is optional). Built, not yet run on a phone
- Galaxy S9, with `mfc=5`, both off by default: `mfc_zc=1` decodes straight into IOSurfaces of the block's memory, `mfc_irq=1` takes the block's interrupt instead of polling. Their one phone run, on an earlier build with both on, had 30 of 300 HEVC frames wrong, cause unknown; this code has not run on a phone

## 2026-11-03

- Galaxy S9, S9+, Note9: at 1125 x 2313 the home screen uses the iPhone X's layout (it was falling to the X's Display Zoom one: smaller icons and widgets, the Search pill and dock close to the icons), with its rows fitted to 771 points, and the status bar's time and battery sit further from the panel's corners. `libhakios.sb.dylib`; not yet run on a phone.
- Galaxy S9: a passcode: Settings can set, change and remove one, the lock screen asks for it after a lock and after a restart, with iOS's delays after wrong codes (never a wipe). There is no Secure Enclave: it gates the screen and encrypts nothing (docs/passcode.md). The RAM disk needs `passcoded` and its job, which stand in for keybagd; `passcode_reset=1` discards a stored passcode
- Galaxy S9: the iris camera (S5K5F1 on CSIS3) and its IR LED, off unless asked (`iris_s9=N`, in no default line): the sensor answers with `iris_s9_smc=1`; no frame has reached memory yet. `fp_s9=N` probes the fingerprint sensor and panics the kernel at present
- status bar: UIKit's pink "rdar:45025538" strip is gone on a screen whose width is not in Apple's table: `libhakios.sb.dylib` picks Apple's notch layout for the screen's width
- Galaxy S9 Bluetooth: a paired device stays connected (`libhakios.bluetoothd.dylib`: bluetoothd's iAP PTY is non-blocking, so a write nobody reads no longer stops its stack); each phone uses its own address from EFS (`bt_addr=` sets another, `bt_addr=0` keeps Broadcom's default); `bt_trace=N` or `sysctl kern.hakios.bt_trace=N` logs the H4 packets
- Galaxy S9, S9+, Note9: iOS is drawn in the panel's shape, not the iPhone X's stretched: 1125 x 2313 (375 x 771 points), on by default (`fb_native=0` is the old picture, 1.28 times across and 1.215 down). The RAM disk needs libhakios.sb.dylib from this version, or the status bar is UIKit's pink "rdar:45025538" strip. MobileGestalt's cache on a kept data volume still answers 1125 x 2436.
- Galaxy S9: all eight cores, on by default on the S9 (`cpus=8`, for which the loader lists core 1 and, with `ufs=3`, its own core; the S9+ and the Note9 keep six). Core 1 was never out of reach: the "parked by S-Boot" finding of 29 October came before patch 1504, from a kernel that read core 1's MPIDR as an M3's number; CPU_ON brings it in 1.1 ms. Core 0 stops being the loader's after `ufs=3`'s switch: the loader jumps into the kernel, as CPU_ON would start the core, and the kernel gives it back at the end of a run (patch 1513). While the kernel has it the watchdog, not the loader, catches a whole-system hang; `core0=0` keeps the old way for a run whose hang you want the loader's log of
- Galaxy S9: XNU has the low RAM bank's free memory too, beside its 2 GB window from 0x880000000: about 1170 MB more on the S9 (`hw.memsize` 3375284224 where it was 2147483648; iOS's free memory at idle 2.1 GB where it was 0.8 GB). The loader takes the free pieces of a list of ranges that keeps clear of every reserved, firmware, driver and keep-out range (`extra_ram_claim`), tests them and names them in the device tree (`hakios-extra-ram`); kernel patch 1512 maps them, makes them managed pages after the window's and gives them to the VM before `bsd_init()`. `extra_ram=0` leaves them out. The S9+ and the Note9 (not seen) have it only with `extra_ram=1`, and under QEMU `extra_ram=1` gives the RAM below the payload the same way (the test of it)
- Galaxy S9: the bottom quarter of the screen no longer flashes black with `fb_zc=1`: none of `fb_zc`'s pool slots runs over 0xE0000000 any more, since what is drawn from there up does not reach the panel. PR 165's second slot, 0xDF800000, crossed that line 8 MiB in, which is the last 24% of a 1125x2436 surface, and it showed black on every other frame. The pool is now four 12 MB slots in the free pieces of the gap below that line (0xDEC00000, 0xD7000000, 0xDA400000, 0xDB000000), and `sys/hakios.exynos9810_mem.h` checks at build time that everything the display scans out ends there (`EXYNOS9810_MEM_DISPLAY_LIMIT`)
- Galaxy S9: `abox=8` no longer plays the 3 s 1 kHz step-7 test tone at every boot. Step 7 is passed over on the way to step 8 unless `abox_tone=1` is given. `abox=7` and the console's `abox tone` still play it
- Galaxy S9: Safari's pages load with the M3s running: what a JIT writes (WebKit's CSS selector compiler, JavaScriptCore) is cleaned from the core's data cache before the core runs it. iOS's `sys_icache_invalidate()` never cleans (Apple's cores and the A55s fetch from their L1 data cache; the M3s, LoUU 1, do not), so every WebContent process died with SIGSEGV at its first page; now EL0's cache maintenance by VA traps and the kernel does it with a DC CVAC first (patch 1511, `ucache_trap`, on with `m3_prep=3`; `machdep.hakios_ucache_ops` counts it)
- Galaxy S9: what has been seen working is on by default: the S9's configuration (`devices/starlte/configs/starlte.config`) sets six cores, the kernel's own UFS driver with /private/var in USERDATA, touch, keys, battery, USB console, GPU and both governors, brightness, zero-copy display, the RTC, the front camera, the speakers and Wi-Fi, and `devices/starlte/mkimage.sh` puts them on every image; a run's own arguments go over them by name (`tools/hakios-config.py args --with`; `name=` drops one, `-n` none). No other device changes
- Galaxy S9: the display's `fb_zc` pool moves to 0xDEC00000-0xE18FFFFF (three 12 MB slots): it ran from 0xD7000000 to `secure_camera`, over the camera's frames, UFS's own-mode memory and Wi-Fi's optional DMA window. The S9's physical carve-outs are one table, `sys/hakios.exynos9810_mem.h`, checked at build time for overlaps; the display logs its ranges at probe
- drivers: one `hakios_physmap_wc()` (`sys/hakios.physmap.h`) maps physical memory XNU does not manage, write-combined, for the camera and UFS's own mode, in place of a copy each; the camera's and the keys' log lines start `exynos_camera:` and `exynos_keys:`

## 2026-11-02

- Galaxy S9: a run's end restarts into TWRP again: the loader writes INFORM2/3 right before it restarts the phone with the PMU's software reset (SWRESET) instead of UEFI's ResetSystem, feeds the watchdog while it shows the kernel's log on the screen (it bit there before, and its reset clears INFORM3: the phone came back into BOOT), and keeps the kernel's log in the RAM log through the last write; the USB console's `return` runs off the busy thread-call group and forces the return after 15 s
- Galaxy S9: six cores, the four Exynos M3s next to A55s 2 and 3 (`cpus=6 bootcpu=2 m3_prep=3 pan=0`): the big cluster's PMU fields written before CPU_ON (`m3_prep`), PAN off on a system with ARMv8.0 cores (`pan`), and user space told only the ARMv8.0 features every core has (`isa_floor`, on with `m3_prep=3`)
- Galaxy S9: touch polled at priority 92 (`s6sy761_pri`), so that a finger's moves no longer come in bursts under load; `s6sy761_trace=N` times the first N events
- Galaxy S9: the front camera, stage 1: its supplies and model ID (`cam_s9=N`, off by default)
- diagnostics: `systrace`'s usertrap, userabort, top and ustacks lines also go to the loader's log (the USB log, esslog)

## 2026-11-01

- graphics: the wallpaper on a slow phone (the Galaxy S9, two cores): once the start-up rush is over, PosterBoard is restarted if launchd killed its posters at the 5 s check-in deadline (`poster_retry`, in the default job list; `poster_retry=0` turns it off)
- diagnostics: the layer dump (`com.hakios.layers`) also lists the CoreAnimation contexts that have no window, so chronod, where the home screen's widgets are drawn, can be asked what a blank widget holds; `RC_JOBS=rc.jobs.widgets` is the recipe

## HakiOS, taken on 2026-11-08 (its own dates and numbers follow; Scion's own version stays as the VERSION file says)

## 2026-11-05: HakiOS 2.3.4

- metal: a pipeline CoreAnimation makes at run time, which its library does not hold, gets the nearest variant of the same kind and not one that draws something else. That was the red at the top of the default wallpaper (its head was not drawn), the white band behind the status bar (`darkenSourceOver`), and materials like the Weather widget's
- touch: a finger's lift 5 ms after its last move at most 6, on the phone under real fingers; 88 to 169 before. `touch: N strokes` in the log says so without `touch_lat=1`'s lines
- metal: at the default screen with the right shaders, on the phone: 109.6 frames a second over the bench's gestures, 1.3% of them over 16.7 ms. 120 is not reached: what is left is SpringBoard's main thread at the start of a gesture (up to 100 ms on the bench, up to 300 under fingers), and the GPU at the home button and opening an app
- windows: `tools/metal-run.sh` renders at the iPhone X's layout at 2 pixels a point by default, 750x1624 stretched to the panel, in place of the iPhone XR's screen: at that class a swipe from the bottom edge opened a sheet under real fingers. Not yet seen under real fingers at this one
- ports: `devices/<name>/metal.args` holds a phone's own arguments for `tools/metal-run.sh`, and a phone other than redwood no longer gets redwood's
- docs: the READMEs, the index and the Windows guide name `hakios-metal/` of this tree; the repository beside it says it has moved

## 2026-11-05: 2.3.3

- metal: `hakios-metal/` is in this tree with its history, and `userland/build.sh` links it: one checkout builds everything
- framebuffer: `fb_beam` and `fb_measure` read the display interface's registers only on `sc7280` or with `fb_intf=`: they read SM7325's address on any SoC before
- hakios-metal: a GPU of another chip ID is refused by name, and the window server stays on OpenGL ES
- tools: `tools/metal-run.sh` takes the phone's own arguments from `DEVICE_ARGS=`; [Port a device](docs/porting/index.md) says what Metal needs from a port
- touch: with a finger down every look reads a report; a finger's lift no longer waits up to 169 ms for the look of every 80 ms
- framebuffer: slow frames are filtered at once, so a still screen no longer moves back and forth 10 times a second; nothing is drawn again from a surface the GPU has taken back

## 2026-11-04

- metal: 120 frames a second is not reached: hakios-metal 101 to 103 in page swipes, about 90 in Settings' scroll, 89 to 92 at the home button, 4 to 6% of frames over 16.7 ms; after the finger lifts, 185 of a swipe's 205 frames take 8.3 ms, while it drags 75 to 80 a second; `docs/reference/metal-measurements.md`
- springboard: `wallpaper_freeze=1` makes the wallpaper one picture, from the IOSurface on the panel (`kern.hakios.fb_surface`): home button 92 frames a second, against 28 with `wallpaper_cache=1` and 50 without either; the icons blink once, the wallpaper does not change until the next boot, and the lock screen shows the same picture
- windows: `tools/metal-run.sh` sets `wallpaper_freeze=1` and `metal_defer=1`; an argument in `MORE` takes the place of a default of the same name
- metal: 250 touch samples a second instead of 125 change nothing (100 to 102); `touch_poll_us` stays 4000 in `tools/metal-run.sh`
- springboard: the status bar's legibility gradient (compositing filter `darkenSourceOver`) came out solid white over the top eighth of the home screen, on both Metal paths; it is drawn without the filter now, the wallpaper reaches the top and the status bar reads (2.3.4 found why, and took this out)
- windows: `tools/metal-run.sh` renders at the iPhone XR's screen by default, 828x1792 stretched to the panel: Settings' scroll 118 frames a second, swipes 110 to 115, home button 106, 1% of frames over 16.7 ms; the wallpaper is the picture a boot at the iPhone X's screen kept
- display: `fb_beam=1`, the default: each thread's rows are written behind the panel's scan line (2433 lines, 8338 us), and to the eye the moving bands are gone; it costs no frames (Settings' scroll 91 against 89, home button 89 to 92 against 81, swipes 101 against 99)
- docs: a short README and a front page by what you came to do; `docs/reference/metal.md` and its measurements; every boot-argument of both Metal paths in the catalogue (`tools/boot-args.py` reads `hm_boot_arg_set`)
- licenses: Turnip, AdrenoMetal, metal2vulkan and the plug-in's origin in `THIRD_PARTY_LICENSES.md`; a binary built with `M2V=1` has obligations and is **to verify** before any distribution

## 2026-11-03

- metal: Metal on Vulkan, as UniXNU runs it: `tools/mk-turnip.sh`, `tools/mk-m2v.sh`, `userland/build.sh TURNIP=1 M2V=1`; nothing of it in `prebuilt/`, you fetch and build it
- metal: `vk_test` passes on the phone (`RC_JOBS=rc.jobs.vktest`): Vulkan 1.3.305, a 4 MB fill checked word by word, a submission and its fence wait 2.0 ms on average
- metal: `m2v_test` passes on the phone (`RC_JOBS=rc.jobs.m2vtest`): the device in 134 ms, a 1080 x 2400 clear checked pixel by pixel, a full-screen clear committed and waited for in 1.08 ms
- metal: `metal_m2v=1`: the window server draws through metal2vulkan, correctly since it opens QuartzCore's pipeline library and picks CoreAnimation's shader variants by label: swipes 50 to 57, scroll 41, the GPU 14 to 21 ms a frame; `tu_debug=` and `m2v_utrace=1` say why
- gpu: HakiosGPU's request `0x7d`: the client's own pages as a buffer; Turnip takes any page-aligned pointer through `VK_EXT_external_memory_host` (grey icons and text before)
- gpu: Turnip has `VK_NV_linear_color_attachment` on Darwin, unused so far: the window server's pass has a second attachment that is not linear
- springboard: `wallpaper_cache=1` rasterizes the wallpaper's window: page swipes 85-91 to 97-103, an app's close slower (22-30 against 40); `wallpaper_freeze=2` hides the wallpaper: home button 78 frames a second against 28, swipes unchanged; `wallpaper_freeze=1`'s picture comes out one colour here, and is not used
- metal: `metal_fps=mainpass` prints the draws of a heavy pass, `metal_fps=passes` each pass's attachments
- display: surfaces let go of when their pixels are on the panel, not when the next frame comes: no change in frame rate
- metal: luma_boost is left out when QuartzCore's library has its own (a later system's, off a Mac): it was 83% of the GPU's timed passes; `metal_luma_boost=1` draws it
- metal: `metal_defer=1` records the window server's command buffers on a thread of hakios-metal's own -- a draw costs its render thread ~5 us instead of ~40
- metal: `metal_fps=passes` prints every pass of a slow frame; the full-screen pass of the home screen is 6 to 13 ms of the GPU's ~10 a frame
- display: `fbsubtype=N` with `fbwidth`, `fbheight` and `fbscale` reports another iPhone's screen class (the XR's 828 x 1792 at 2: no red rdar status bar)
- springboard: vibrant views keep their filters when the window server draws through Metal; `ui_lite=1` turns on Reduce Transparency and Reduce Motion, `ui_lite=0` turns them off
- windows: `tools/metal-run.sh` runs on eight cores at 120 Hz with six scaling threads and without `-v` (VERBOSE=1 brings it back), and its summary shows the frames on the panel; `tools/frames.py` counts the Metal road's frames and the share over 8.3 ms
- loader: `xnuloader/mkidentity.sh` builds lzfse by its default target (there is no `lzfse` one)

## 2026-11-02

- metal: the window server's compiled shader stages outlive the boot -- kept in `/var/db/hakios-metal-cache` (var_store left the old `/var/tmp` out), keyed by what the compiler is, so a rebuilt plug-in keeps them; and carried in the image (`tools/metal-cache-seed.sh`), so no boot compiles them
- metal: `metal_fps` says why each pass stayed in sysmem, the slowest and the heavy one included
- windows: `tools/metal-run.sh`, a measured Metal run in one command from Git Bash (`var_store` on by default); `tools/ca-metallib.py` off a Mac with `CA_QC_METALLIB=`

## 2026-10-30

- repositories: `main` is the branch of `hakios-xnu-kernel` -- what was `hakios` and then `u-boot`; `hakios` is gone, `git clone` needs no `-b`
- site: a Changelog page, this file
- site: the commands to start with clone the public repository (`hakios-xnu-kernel`); `oosexgang/hakios` is private and cloned for nobody
- site: the documentation synced, with the guide to installing beside Android and the U-Boot page in the table of contents

## 2026-10-29

- u-boot: HakiOS's own U-Boot, started by the phone's bootloader from RAM; it starts the loader and the kernel from memory, nothing of the vendor's firmware in between
- u-boot: the loader's fastboot works under U-Boot (USB 2.0 without the link power states that dropped the cable after 3 s, endpoints and packet sizes as the loader asks, DWC3 transfers started when queued)
- u-boot: every download and every step says what it does on the screen, tagged `[oosexgang]`
- u-boot: `hakios sum` checks a flashed partition from the phone itself; `hakios reboot bootloader` gets to fastboot
- install: HakiOS beside Android: U-Boot in `boot_a`, Android in slot b untouched, a menu at every start (Android after 10 s, HakiOS (Darwin), fastboot, U-Boot's shell)
- install: choosing Android marks HakiOS's slot unbootable and lets the bootloader switch; U-Boot never changes which slot is active (changing it by hand once left every image refused with "Device Error")
- install: `tools/userdata-carve.py --wipe` makes `darwin` at the end of userdata; `tools/sparse-split.py` cuts an 8 GB image into pieces Windows fastboot can send
- storage: the data volume is HFS Plus in `darwin` after the system, about 12 GB, journaled, kept between starts; before, `/private/var` was a memory volume shown as 1 GB and empty at every start
- storage: HFS Plus built into the kernel; Apple's sources are fetched at `xnu/apply.sh`, only this port's changes are kept
- storage: the data volume's writes go straight to the flash and the cache is flushed when the journal asks (a flush and a read back after every write made the system crawl)
- storage: the loader takes any phone's `darwin` (name, type, unit, at least 20 GiB), not only the first phone's place and identifier
- storage: `tools/mkdatavol.py` makes the empty volume as a 50 KB image that leaves the system volume alone
- graphics: the installed image draws with the GPU (Mesa, `GL=1`): smooth, and the wallpaper drawn
- graphics: the system's other services and the processes that draw parts of the interface are in the image (`REST=1`): the wallpaper and the applications that need them
- graphics: frames at 120 a second (`fb_hz=120`), and `fb_measure=1` writes to the log how often the panel itself refreshes
- loader: nothing printed over the system's screen while it runs; the log still takes every line
- loader: its log written under U-Boot too; a download the computer gave up on no longer hangs it
- wifi: the Wi-Fi controller is found where the system looks for its address, so registering applications no longer waits 5 s for each (found by the ios-sapphire port)
- tree: Darwin, not iOS, in the sources, the scripts, the file names and the documentation; no file of Apple's in the repositories
- docs: installing beside Android, step by step (`docs/dual-boot.md`, in Russian too), and the U-Boot page with everything as it was done and checked
