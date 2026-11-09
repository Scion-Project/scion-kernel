# iOS 27 on the Galaxy S9: boot time and trap cost (9 November 2026)

What was measured on one S9, between midnight and 03:00 UTC on 9 November 2026, to make the iOS 27 boot shorter and the UI less starved, which of it
is kernel and which is root, and what is still slow. The kernel changes are in this repository behind a boot-argument; the root edits are tools you
run on your own root image; nothing of the builder's images, caches or logs is here.

**What is verified, and how.** The numbers below were taken on a phone with a kernel (`kernel-speed3`) that had the changes on unconditionally, and a root
edited as described. That kernel was Starbite's merged kernel from `main` after #255 plus a 14 KB diff. This repository carries the same diff in two parts,
`xnu/files-12377/osfmk/arm64/sweet_a64e.c` and `bsd/dev/starlte_smp.c` (new files of the port) and `patches/xnu-12377/starlte/0426`, with one change: each
behaviour is behind a bit of the boot-argument `sweetspeed=<mask>` (default 0, nothing changes), as this repository ships behaviour changes. The applied tree
was compared with the speed3 sources file by file: they differ only by those switches. With `sweetspeed=31` the code is the code that ran. **The gated
kernel itself was built (the iOS-target STARLTE kernel, as for #255) and not booted**; `tools/ios27/cmdline/starlte-fast.txt` is the command line of the best
boot below with `sweetspeed=31` added (it now has `sweetspeed=2047`, with the night run's bits of `NIGHT.md`, and `sweettrace=0`, the default, is out for room) (and `a5fb_noconsole=1` taken out: the STARLTE framebuffer driver reads `sweetfb_noconsole` only, and the line is limited
to 607 bytes).

## The boots

One change set each, one root each. "Display" is the time from the reboot command to the first frame.

| Boot | Kernel | Root | Result |
|---|---|---|---|
| m3c (before) | #255's, all eight cores | the GPU track's | 4-13 fps; Spotlight alone 600 thousand PAC traps a second; `backboardd` 59 s of CPU compiling Metal shaders; display 72 s |
| s1w | same | 86 daemons off (`trim-s1.list`), `backboardd` out of the Metal clients, wallpaper from a static image | 6-23 fps; wired memory 330 MB against 780 |
| s2 | same | the Metal track's new plug-in | stable; showed a 29 s gap in the kernel's boot |
| s3 | s3 kernel: the soft M3 pins (0425) and the diagnostics (pin and A64E hot-spot lines, `SBBOOT` stage times) | same | found the 30 s IORTC wait, PACGA at 800 thousand traps a second, and the FP16 instructions that pin threads |
| s4 | s3 plus no IORTC wait, FP16 FMOV/FCMP emulated on the M3s, PACGA as AND | the shared cache's PACGA and `LDRAA/LDRAB` rewritten ahead of time (`edit-s4.sh`) | display 33 s after the reboot command (s3: 72 s); PAC traps about zero at idle |
| s5-s6 | s3 plus a sampler (not here) | s4 plus the Settings patch (`edit-s6.sh`) | found and fixed the Settings freeze |
| s7 | the s4 kernel | s6's root | the best boot: display on 35 s after the command, idle CPU 3-5 % at 80 s |

## What each change does

All of the kernel's are in `sweetspeed=<mask>`; `xnu/files-12377/osfmk/arm64/sweet_a64e.c` has the table.

| Bit | Change | What it bought |
|---|---|---|
| 1 | `IOKitInitializeTime` does not wait 30 s for the `IORTC` resource (`iokit/Kernel/IOStartIOKit.cpp`; no driver on this port publishes it) | 30 s of every boot |
| 2 | on a Mongoose M3: `FMOV Hd,#imm`, `FMOV` between H and W/X, `FCMP/FCMPE` H are emulated (moves and compares: exact) instead of pinning the thread to an A55 | the FP16 pins of iOS 27's UI code, which was the reason threads did not stay on the big cores |
| 4 | `PACGA` gives `Xn & 0xffffffff00000000`, so that the trapped ones agree with the cache's rewritten ones | with the rewrite, PACGA stops trapping |
| 8 | `SBBOOT <ms> ms: <stage>` lines from `kernel_bootstrap`, `kernel_bootstrap_thread` and `bsd_init` | how the boot time was found |
| 16 | `STARLTE PIN: <program>[pid] ...` for each of the first 64 thread pins, then one every 10 s at most | which program, and which instruction, pins |

Always on, as diagnostics that do not change what runs: a table of the 512 hottest non-LSE emulated user PCs with their instruction words, printed by the
`startop=1` report as `STARLTE A64E hot:`.

**Order matters for bit 4.** The root rewrite (`edit-s4.sh`, the `PACGA` to `AND` rewrite of the cache) and the kernel's bit 4 must be on together: a
rewritten site and a trapped one would otherwise give different values for the same input. To go back to a kernel without bit 4, put the original cache
files back first (copy the subcaches before you rewrite: the script prints which ones it changes).

## The root

All of it is on your own root, with tools that read nothing of Apple's except the root you give them. `tools/ios27/speed/`:

| Tool | What |
|---|---|
| `a64e-rewrite2.py` | `tools/ios27/a64e/a64e-rewrite.py` plus the `PACGA` and `LDRAA/LDRAB` with writeback rewrites. It leaves libcorecrypto alone (the default of `--skip`) |
| `edit-s4.sh` | attaches a raw root image and runs it over the cache |
| `trim-s1.list`, `edit-s1.py`, `edit-s1.sh` | the 86 launchd jobs that the S9 does without (Siri, Spotlight indexing, intelligence and analysis daemons, Photos analysis, Music, Health, iCloud sync, SensorKit...) moved into `launchd-disabled.plist`; `backboardd` taken out of `mtl2-clients` |
| `cachepatch.py`, `edit-s6.sh` | patches one word of the cache at an address, only if it is the expected one: the Settings freeze patch |

**The Settings freeze.** Preferences' main thread blocks in `SCNetworkInterfaceCopyAll`, in `__wait_for_IOKit_to_quiesce`, in `SCDynamicStoreNotifyWait`:
configd's InterfaceNamer never reports IOKit quiet on this kernel. SystemConfiguration's `____wait_for_IOKit_to_quiesce_block_invoke` starts with
`pacibsp` (unslid address `0x1ab5bc514` in the iOS 27.0.1 cache the work used); changing that word to `ret` ends the wait. The real fix is the IOService that
keeps IOKit busy; it was not found.

**Not published.** An earlier step rewrote libcorecrypto too, which makes its FIPS self-test fail, and the work ran for a while with a locally made stand-in
for the test's helper. That is a local, optional step the builder took on their own root: this repository has no stand-in, no way to make one and no tool that
needs one (`a64e-rewrite2.py` skips libcorecrypto unless told not to), and `docs/ios27/EIGHT-CORES.md`'s rewrite skips it too. Root images, the originals of
every cache file the tools change, the work's logs and the boot images are not here either: they carry the builder's own IPSW and identity.

## What was found and is not fixed

- **A boot dance** (every boot but s1w; found and fixed afterwards, `NIGHT.md`: launchd's kqworkloop limits killed the daemons, `sweetspeed&256`): SpringBoard is relaunched every 5 s four times (`ThrottleInterval`), and `backboardd` is killed twice at about 10 s
  while still in dyld (fixing up the non-cache dylibs `s.dylib`, `libsbgl`, `libOSMesa`, 16 MB, on a cold boot); the third `backboardd` reaches the display
  1.3 s after exec. It costs about 20 s. Who kills it is unknown (no crash, no launchd message). Ideas: warm those libraries into the page cache before
  `backboardd` starts; find the 10 s killer.
- **GPU submits are synchronous** (`NIGHT.md` has asynchronous ones behind `sweetspeed&64`, submit 48 us against 165-192 us, frame rates unchanged): StarbiteMali's `SBM_SUBMIT` runs the job chain to completion, polling; 2-3 thousand submits a second while
  scrolling, 27-37 % of `backboardd`'s time. The present copy is 2.3-2.6 ms a frame (under 3 % at 10 fps) and is not the bottleneck; zero-copy scan-out
  to the DPU was not tried.
- **Crash loops** left in the best boot (`NIGHT.md`: `imagent` and `terminusd` disabled, the `kbd` crash patched out of the cache): `MessagesBlastDoorService` every 10 s (imagent), `terminusd` at boot, and `kbd` on the first use of the keyboard
  (a null dereference in SafariCore's `WBSSetUpAccessToAppDataContainerWithIdentifier`, reached through the one-time-code autofill provider: Safari's app
  data container is missing).
- `sweettrace=0` alone does not stop the pid 1 system call trace of this kernel; the command line keeps `sweettracename=none` too.

## Reproducing it

    # the kernel: the ios-27 profile's tree (docs/ios27-starlte.md), then the build of tools/ios27/build-xnu-12377-ios.sh
    # the boot arguments:  tools/ios27/mkpayload-ios26.sh NAME tools/ios27/cmdline/starlte-fast.txt
    # the root:            tools/ios27/speed/edit-s1.sh ROOT.img ; tools/ios27/speed/edit-s4.sh ROOT.img ; tools/ios27/speed/edit-s6.sh ROOT.img

The command line is `starlte-8core.txt`'s neighbour: the eight-core set with `cpufreq_gov=1 cpufreq_gov_big_min=1794000` (the governor of `starlte/0421`),
`sweetm3unpin=100` (the soft pins, `0425`), `sweettrace=0 sweettracename=none`, the Metal client list of `docs/ios27/METAL.md` and `sweetspeed=31`.
The unpin and the governor were on in the later boots above; their effect alone was not separated from the rest.

The next run, with a profiler, is in `OPTIMISE.md`.
