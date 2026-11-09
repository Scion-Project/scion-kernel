# iOS 27 on the Galaxy S9: the night run, kills, pacing, brightness (9 November 2026)

What two runs on one S9 found between 01:40 and 04:45 UTC on 9 November 2026 after the speed work (`SPEED.md`): why the boot restarted
SpringBoard, `backboardd` and thirty daemons, what the 10 frames a second of animations are, what Metal in CoreAnimation cost, how iOS 27
sets the panel's brightness, why the wallpaper stays a static image. The kernel changes are in this repository behind a bit of the boot-argument
`sweetspeed=<mask>` (default 0: nothing changes); the root edits are tools you run on your own root image. Nothing of the builder's images,
caches, roots or logs is here.

**What is verified, and how.** The numbers below were taken on a phone with kernels that had these changes behind a different boot-argument (`sbx`, one
bit each, the command line being at its 607-byte limit) and a root edited as described. This repository carries the changes as
`commit 0427 of scion/xnu-12377-starlte` (the hooks in Apple's and the port's files) and `0428` (Scion's S6E3HA8 panel driver, `drivers/display/panel`, taken to this
tree), with the bits renumbered into `sweetspeed`. The applied tree was compared with the tree that ran, file by file: they differ only by the boot-argument
switches, comment wording, and the panel driver, which is Scion's current one (the phone ran an older copy of it; the new functions are the same). **The gated
kernel was built (the iOS-target STARLTE kernel, as for #255 and #260) and not booted.** What the phone showed is the measured part; "should" below is a guess.

## The bits

`scion/xnu-12377-starlte:osfmk/arm64/sweet_a64e.c` has the table; 1 to 16 are in `SPEED.md`.

| Bit | Change | Where |
|---|---|---|
| 32 | `SWEET SIG: <sender>[pid] -> <target>[pid] sig N by kill\|terminate_with_reason\|psignal`, `SWEET EXIT: ...` for every process exit, `SWEET SIGBT:` (a kernel backtrace of every SIGKILL a process sends itself), `SWEET LIMITS:` (launchd's spawn limits per exec), `SWEET RLIMIT:` (a port, file or kqworkloop limit hit) | `kern_sig.c`, `kern_exit.c`, `kern_exec.c`, `task.c` |
| 64 | asynchronous GPU submits for StarbiteMali clients that ask (selectors `0x105` async, `0x106` wait for idle) and the framebuffer's `HakiosWaitsForGPU` property | `starlte_gpu.c`, `StarbiteMali.cpp`, `SweetFramebuffer.cpp`, and the Mesa side in `tools/ios27/mesa-ios/starbite-drm` |
| 128 | `SWEET PREWARM`: a kernel thread reads the GL stack and the UI daemons (about 18 MB) into the page cache once the root is mounted | `bsd_init.c` |
| 256 | the kqworkloop limits launchd sets at spawn are ignored | `kern_exec.c` |
| 512 | the S6E3HA8 panel driver (as `panel=1`); iOS 27's brightness, sent with each frame, goes to it; `setProperties` keys named `brightness*` do too; `SWEET FB ARGS` lines for the framebuffer selectors seen rarely | `SweetFramebuffer.cpp`, `hakios.s6e3ha8.c` |
| 1024 | the framebuffer's 60 Hz timer keeps an absolute deadline | `SweetFramebuffer.cpp` |
| 2048 | CoreAnimation pacing aids, as `sweetfb_cav=1`: `IOMFBDisplayRefresh`, `SWEET FB CAV` lines | `SweetFramebuffer.cpp` |
| 4096 | scripted input for a repeatable frame-rate run | `starlte_touch.c` (script 5) |
| 8192 | scripted input that drags the brightness slider (blind) | `starlte_touch.c` (script 6) |

The best boot below ran with 1, 2, 4, 8 and 16 of `SPEED.md` always on and bits 32, 64, 128, 256, 512 and 1024: `sweetspeed=2047`, which is what
`tools/ios27/cmdline/starlte-fast.txt` now has.

## The boots

One change set each (n2 changed the root and added the prewarm in the same boot); p1 on the root of `SPEED.md`'s s6, n2 on with the edits below.

| Boot | Kernel | Result |
|---|---|---|
| p1 | s7 plus the kill log (32) and the scripted input (4096) | 96 self-sent SIGKILLs at boot; scripted bench: median 20.6 fps (swipes 33.2, scroll 17.1, idle 13.1); first frame 28.5 s after the kernel started |
| n1 | p1 plus the SIGKILL backtrace | every kill is `task_fatal_port_no_senders` from a kqworkloop limit notification (below) |
| n2 | plus 256 and the prewarm (128), and the root edits of `edit-p2.sh` | 0 self-kills, every daemon exec'd once, first frame 15 s |
| n3 | plus async GPU (64) | stable, 0 GPU failures; submit 48 us (synchronous: 165-192 us); swipes 33.0, scroll 16.9, idle 10.0; prewarm read 17 MB in 569 ms; first frame 13.8 s |
| n4-n5 | plus the panel driver and brightness (512) | stable; the driver read the panel's ID, MTP and gamma; no brightness set at boot |
| n6 | n5 with Metal in CoreAnimation | slower (below), reverted |
| n7 | n5, GL, and the QuartzCore clamp patch | swipes 34.1, scroll 17.7 (p90 32.3), idle 10.0 |
| n8 | plus 2048 | CA's power state "on", yet 10-11 swaps a second at idle with SpringBoard at 1 % CPU: the 10 Hz is the content; while swiping 41-52 swaps a second reach the kernel but only 33-36 are presented |
| n9 | plus 1024 | swipes 36.6 (p90 37.2), scroll 19.6 (p90 27.3): presents now follow swaps |
| n10 | plus the PosterBoard store moved aside, the slider script | stable; slider test inconclusive; PosterBoard still starts no poster extension |
| final | 32, 64, 128, 256, 512, 1024 | 0 self-kills, SpringBoard and `backboardd` exec'd once, the timer exactly 60 Hz (300 ticks in 5000 ms), idle CPU 2-5 % at the lock screen |

Frame rates are the framebuffer's own `fps=` lines in the scripted windows (16 home-screen swipes, 16 scroll swipes in Settings); the kernel changes did
not make the UI fast, they removed the boot dance and the stalls between presents.

## The kqworkloop kill chain

At every boot of `SPEED.md`'s s7 SpringBoard was relaunched four times, `backboardd` twice and about thirty daemons (`runningboardd`, `lsd`...) once or
more, 20 to 100 ms after exec: no crash report, no launchd message. The kill log shows each as a SIGKILL the victim sent itself, and the backtrace of it as:

1. launchd gives each daemon `posix_spawn` resource limits: kqworkloops with a hard limit of 10 (some 5, 3, 1), a thread limit of 32, port and file limits.
   The kernel logs them with bit 32 (`SWEET LIMITS`).
2. On the 11th kqworkloop a daemon creates, the resource AST (`CONFIG_PROC_RESOURCE_LIMITS`, `task_set_kqworkloop_limits`) builds a "fatal port",
   sends the hard-limit notification, and drops the port.
3. Nothing here receives that notification, so the port's last send right goes: `task_fatal_port_no_senders` -> `task_identity_token_try_sigkill`. The
   SIGKILL is sent from inside the victim's own context, which is why it looks like a self-kill.

Bit 256 zeroes the kqworkloop limits at exec. This cost about 13 s of every boot and was the "who kills `backboardd`" of `SPEED.md`'s unsolved list;
the page-cache prewarm (128) was the first guess (dyld fix-ups of the 16 MB of non-cache dylibs on a cold boot); it went into the same boot as
256, so its own effect was not separated: it read 17 MB in 569 ms. XNU 8796 (iOS 16.5) has the same fatal-port path; whether Scion's 16.5 launchd sets those limits was
not checked.

## Pacing

iOS 27's `backboardd` never uses IOMobileFramebuffer vsync (notification 5) or FrameInfo (3). It runs a CADisplayTimer at 60 Hz in software, and the
framebuffer's own timer was a second, separate cadence.

- **The framebuffer timer** re-armed itself `setTimeoutUS(16667)` after each present, so a tick took about 19-20 ms (the present is about 2.4 ms, plus
  timer slack), 33-36 presents a second at best while CoreAnimation swapped 41-52. Bit 1024 keeps an absolute deadline (`next += 16.667 ms`; when late,
  the next tick is `now + interval/8`, no burst) and `wakeAtTime`s it. Swipe median 34.2 -> 36.6 fps, scroll 17.7 -> 19.6. Not verified on a phone
  with the gated tree.
- **The 10 Hz of animations and idle redraws.** `CA::WindowServer::Server::render_for_time` clamps the next update to at least now + 0.1 s unless the
  display is internal and its target power state is on (bytes 1 and 5 of the display state) or a power assertion is held. `backboardd`'s
  `RequestPowerChange` (IOMFB selector 12) went 1, 0, 1 at +11 s and +16 s after the first frame (the kernel ignores the 0, `a5fb_keep_on`, but CoreAnimation sees it); a
  client commit (touch) bypasses the clamp, which is why swipes reach 33-37 fps while scroll deceleration and animations run at exactly 10. The patch
  (`edit-n7.sh`, below) takes the `tbz` that tests that state and makes it an unconditional branch. On the phone it did not change the idle 10 Hz: bit 2048's
  log said the power state was on and SpringBoard was at about 1 % CPU, so the idle 10 Hz was content (a spinner-like animation), not a cap. The
  patch is harmless and left in; the cause of the 10 Hz while scrolling decelerates is open.
- iOS 27's IOMFB selectors `backboardd` uses: 3 GetLayerDefaultSurface, 4 SwapBegin, 5 SwapEnd (a `0x6e0`-byte record), 6 SwapWait, 8 GetDisplaySize,
  12 RequestPowerChange, 15 SetDebugFlags, 17 SetGammaTable, 18 IsMainDisplay, 22 SetDisplayDevice, 25 GetDigitalOutState, 27 GetGammaTable,
  29 GetDisplayArea, 33 Enable/DisableVideoPowerSavings, 50 SetBrightnessCorrection (16.16), 55 SetMatrix, 57 GetColorRemapMode, 68 SetParameter,
  72 Enable/DisableNotifications (only type 9, ReqDCPReset), 74 SupportedFrameInfo, 79 GetBlock, 84 SetContrast, 88 OpenByName. Notification
  types: 0 HotPlug, 1 Power, 3 FrameInfo, 4 CRC, 5 Vsync, 6 NeedSwap, 7 Relbufinfo, 8 ProxSensing, 9 ReqDCPReset, 10 APTQuantaSwitch, 11 GenlockError.

## Metal in CoreAnimation (tried, slower)

n6 put `backboardd` into the Metal clients list (`mtl2-clients`, `SWEET_MTL2_DEXR=1`, `sbgl=0`) so CoreAnimation drew with Metal instead of GL.
It worked and was slower: swipes 25.7 fps (n4: 34.8), scroll 10.9 (17.7), first frame 27 s (13.8-15). n7 undid it. GL stays the default; the
on-disk shader cache `/var/local/m2v-cache/<program>` was filled by that run.

## Brightness

iOS 27's CoreAnimation sets the panel's brightness per frame: `IOMobileFramebufferSwapSetBrightness` sets a byte in the swap record and stores the
brightness, a double, in it; from `kern_SwapSetBrightness` the record (the framebuffer object plus `0x18`, sent by SwapEnd) has the flag at `+0x354` and the
double at `+0x35e`. With bit 512 the framebuffer decodes it on change (integer arithmetic, no floating point in the kernel): a value up to 1.0 is a fraction of
255, above that nits, taken through the S6E3HA8 table (`hakios_panel_brightness_nits`: the lowest level whose luminance row reaches it), with a floor of
16/255 so that a misread value never leaves the panel dark; `SWEET FB: swap brightness X -> panel level N` says what it did. A `setProperties` of a
number under a key that starts with `brightness` (iOS 16.5's path) goes to the panel too. The panel driver is `drivers/display/panel/hakios.s6e3ha8.c`
as it is for iOS 16.5, taken to this tree by `0428`; on the phone it read the panel's ID, MTP and gamma, and the calculation agreed with Samsung's.

**Not seen working**: the slider was not moved on the phone (the scripted drag drew a few frames and no swap brightness line). If nothing logs when the
slider moves, look at the `SWEET FB ARGS` lines (selectors 50, 68, 84) and whether CoreAnimation calls SwapSetBrightness on this display at all.

## The wallpaper

PosterBoard does not start a poster extension, so the home screen shows the static `sbwall` image of `SPEED.md`. Its data store is at schema version 59
on this root and iOS 27.0.1 needs 61: the log says "Failed to migrate to Version 61 from 59" and then "Data store could not be created". Moving the store
aside (`edit-wall.sh`) so it is made new did not start the extension either, so something else is missing. Open.

## Async GPU submits

StarbiteMali's `SBM_SUBMIT` used to run the job chain to completion, polling (`SPEED.md`: 27-37 % of `backboardd`'s time while scrolling). A client that asks
(`SBM_ASYNC`, selector `0x105`, only with bit 64) now has its chain started and the call returns; one chain is on the GPU at a time, in order, and the next
submit, a wait (`WAIT_BO`, the new `0x106` wait for idle, a fence wait, `GEM_CLOSE`) or the kernel's own jobs (the present) finishes it: polled, 1 us and
then 20 us steps, 2 s limit and then a GPU reset as before. A per-buffer "last chain" number makes `WAIT_BO(timeout 0)` honest, so Mesa's buffer cache keeps
working. `HakiosWaitsForGPU` on the framebuffer stops hakios-gl's `glFinish` per frame. A line every 10 s: `STARLTE GPU ASYNC (10 s): N chains, gpu avg X us
(max), next submit waited K x avg Y us`. Submit time on the phone: 48 us against 165-192 us; the frame rates did not move (n3), so, as a guess, the GPU was not
what the scripted windows waited for. The Mesa side is the `starbite-drm` shim: it asks for async unless `STARBITE_DRM_SYNC=1`; a syncobj wait becomes a wait for
idle with the absolute timeout turned into a relative one. Without bit 64 the kernel answers a client that asks with "synchronous", and the new library on
an old kernel stays synchronous.

## The root

Tools in `tools/ios27/speed/`, each of which attaches a raw root image on the Mac, saves what it changes to a directory (`ORIG`) and detaches:

| Tool | What |
|---|---|
| `edit-p2.sh`, `disable-p2.list` | the perf2 edits: the `kbd` cache patch below, `com.apple.imagent` (it starts `MessagesBlastDoorService`, which crashed every ~10 s) and `com.apple.terminusd` (SIGTRAP three times at boot) moved into `launchd-disabled.plist`, and, given a path, a rebuilt `libOSMesa.8.dylib` |
| `edit-n7.sh` | the QuartzCore clamp cache patch below |
| `edit-wall.sh` | the PosterBoard store moved aside |
| `cachefind.py` | which subcache holds an address (to save it before `cachepatch.py` changes it) |
| `bench.py` | one boot from a timestamped console log: the frame rates of the scripted windows (median, p10, p90), exec counts of daemons, crashes, self-sent SIGKILLs |

### Cache patches (`cachepatch.py`)

One 32-bit word each, at an unslid address of the iOS 27.0.1 (24A446) shared cache the work used; `cachepatch.py` refuses when the word there is not the
expected one, so another build is told, not patched. Both are in `edit-p2.sh` / `edit-n7.sh` with their defaults and are the only cache edits of this run.

| Patch | Address | Word | What |
|---|---|---|---|
| `kbd` stub | `0x1aa75d000` (SafariCore, `_WBSSetUpAccessToAppDataContainerWithIdentifier`, a void function; subcache `.11`) | `d503237f` (`pacibsp`) -> `d65f03c0` (`ret`) | `kbd` crashed on the first use of the keyboard: the one-time-code autofill provider reaches that function, whose error path dereferences NULL when Safari's app data container is missing. Returning at once ends the crash; autofill of that kind does nothing. Not tried by an owner typing yet |
| QuartzCore clamp | `0x18441a8bc` (`CA::WindowServer::Server::render_for_time`) | `360002b4` (`tbz w20, #0, +0x54`) -> `14000015` (`b +0x54`) | never takes the 10 Hz "display not internal and on" clamp; see Pacing: no effect on the idle 10 Hz on the phone |

To go back, run `cachepatch.py CACHEDIR ADDRESS NEW OLD`, or put the saved subcache (`ORIG`) back.

## What is left

- `MessagesBlastDoorService` still SIGTRAPs in QuartzCore (launchd backs off, six or seven times per boot) and `chronod` 0-1 times; `mtl27test`, the Metal
  track's one-shot job, runs once about 90 s after boot (25 % CPU for 10 s).
- The brightness slider and the keyboard (`kbd` stub) need someone at the phone.
- The idle 10 Hz: content, not a clamp; what draws it was not found.
- The gated kernel has not been booted. A first boot: `sweetspeed=2047`, then `bench.py` on the console log with bit 4096 for a scripted run, and compare
  with the table above.

## Reproducing it

    # the kernel: the ios-27 profile's tree (docs/ios27-starlte.md), the build of tools/ios27/build-xnu-12377-ios.sh
    # the boot arguments:  tools/ios27/mkpayload-ios26.sh NAME tools/ios27/cmdline/starlte-fast.txt      (sweetspeed=2047)
    # the root:            tools/ios27/speed/edit-p2.sh ROOT.img [libOSMesa.8.dylib] ; tools/ios27/speed/edit-n7.sh ROOT.img ; tools/ios27/speed/edit-wall.sh ROOT.img
    # a diagnostic boot:   add 4096 to sweetspeed for the scripted input, 2048 for the CoreAnimation lines; python3 tools/ios27/speed/bench.py LOG START_EPOCH

Way back: kernel only, `sweetspeed=31` (`SPEED.md`'s best boot, on this root too) or without 64 for synchronous GPU submits; root, the cache patches by
`cachepatch.py` with the words swapped or the saved subcaches, `launchd.plist` and `launchd-disabled.plist` from `ORIG`, the saved `libOSMesa.8.dylib`.

The next run, with a profiler and the changes made against its cost table, is in `OPTIMISE.md`.
