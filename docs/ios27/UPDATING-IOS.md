# Moving iOS 27 on the Galaxy S9 to another iOS build

Scion's iOS 27 line is made for **iOS 27.0.1, build 24A446**, the iPhone 11's (iPhone12,1) restore image. Apple's current
release for that phone is **iOS 27.2.1, build 24B156** (9 November 2026, the only build Apple signs; Apple's catalogue,
api.ipsw.me). This page says what in the tree belongs to 24A446, what is likely to be different in 24B156, what only the
new restore image can answer, and the steps of a move, with who does each and the disk each needs. Written on 17 November
2026, first from public sources only. Later that day the owner's own 24B156 restore image was on the Mac mini, and the
section "24B156: what was found" was read from it. There was no phone: **nothing on this page has been run on 24B156.**

The tools now take the build from one place (`tools/ios27/builds/`, below); the build they use is still 24A446 and what
they make for it is unchanged.

## The short version

- **Kernel.** 24B156's kernel is `xnu-13432.42.3~28` (Darwin 27.2.0, read from its kernelcache); 27.0.1's is
  `xnu-13432.2.10`. Apple has published neither. Scion's kernel is xnu-12377 (iOS 26) with Darwin 27's eight system calls
  added. Compared with the fork's tables, 24B156 adds or renumbers no system call or Mach trap. It serves two MIG routines
  more: IOKit's `io_service_get_wait_info` and `mach_memory_entry_region_info`, which Scion's kernel answers with
  `MIG_BAD_ID`.
- **User space.** Every address Scion uses in Apple's shared cache moves (QuartzCore alone grew by 31 KB of code). In
  24B156 these were found again by name:
  - four of the five one-word patches: `tools/ios27/s9/patches-24B156.txt`, checked by the repository's own tools;
  - all 14 QuartzCore addresses of `sbca`, with the words it checks and its offsets unchanged:
    `tools/ios27/builds/24B156-sbca.txt`;
  - the 136 shader names, which are unchanged.

  The CoreImage word was not carried over.
- **Effort.** With the 24B156 findings below, about 6 to 12 working days. 4 to 7 of them need a phone. Most of the
  remaining work is getting the picture back to where 24A446 is now.
- **Disk.** The move needs about 100 GB at its peak while 24A446 is kept beside it (section "Disk"). The Mac mini had
  42 GB free in the morning and about 96 GB after a cleanup. The 24B156 inputs take 15.5 GB.
- **When.** After the rendering and performance work that is open on 24A446 has landed. The parts that need no phone can
  be done in parallel once there is disk for them.

## The build manifests: `tools/ios27/builds/`

| File | What it is |
|---|---|
| `DEFAULT` | One line: the build every tool uses when nothing says otherwise. Today `24A446`. |
| `24A446.conf` | What the tools need to know of 24A446: version, product, device class (`n104ap`), the restore image's name and size, Apple's kernel version, the three images' names (`build-full-root.sh`'s defaults), the patch list (`s9/patches-24A446.txt`), `STATUS=supported`. `KEY=VALUE` lines, read and never executed. |
| `24B156.conf` | The same for 24B156, `STATUS=candidate`: the image names from its manifest, the kernel read from its kernelcache, and a first patch list (`s9/patches-24B156.txt`, four of the five words). |
| `24B156-sbca.txt` | Candidates for sbca's 24B156 block (`sbca.c` is left alone: the 24A446 line uses it). |
| `build-conf.sh` | Sourced by the tools: `ios27_build_default`, `_get`, `_check`, `_patches`, `_of_inputs` (from `inputs/SOURCE.txt`), `_of_root` (an image's `SystemVersion.plist`), `_same` (refuses a mixture). bash 3.2. |
| `builds-test.py` | CI: the manifests parse; for 24A446 the library gives exactly the values the scripts had written in them; the refusals below happen. |

`IOS27_BUILD=<build>` picks another build that has a `.conf` (under `sudo`, name it on `sudo`'s line). A build whose
STATUS is not `supported` is named in one line on stderr.

**What refuses what:**

- `fetch-apple-inputs.sh`: a build with no `.conf`. A restore image of another build than the one asked for. That second
  refusal is as before, and now names the builds that have a `.conf`.
- `prepare-root-inputs.sh`: a build with no patch list. A cache whose expected words are not there (as
  before). It also checks a mounted image whose `System/Library/CoreServices/SystemVersion.plist` names another build. It
  writes `BUILD` into the private directory.
- `build-root.sh`: inputs whose `SOURCE.txt` names another build than `IOS27_BUILD` or the private directory's `BUILD`.
  It passes the build on as `BUILD_VERSION`.
- `build-full-root.sh build`: refuses before the cache pipeline when any of these differ from `BUILD_VERSION`:
  - the private directory's `BUILD`;
  - the OS image's own `SystemVersion.plist`;
  - a reused cache's `work/cache/DONE` (which now records its build).

  `BUILD_VERSION` goes on, as before, to the herolte tuning as `LastSystemVersion`.
- `check-root.sh`: fails a root whose `SystemVersion.plist` and `LastSystemVersion` differ. backboardd would run a data
  migration at the first boot.

Older private directories, inputs and caches carry no record, and they are taken as before. For 24A446 nothing that is made
changes:

- The library gives the same build, class, patch list, image names and refusal text (`builds-test.py`).
- `fetch-apple-inputs.sh` and `prepare-root-inputs.sh` were run before and after this change on a made-up restore image
  and a made-up cache. Both were made for the test: a manifest of our own and dummy files, nothing of Apple's. The logs,
  the files and `patches.txt` were the same byte for byte.
- **Not run:** `build-full-root.sh build`, which needs `sudo`. Its new checks were read and parsed, not executed. The check
  of a mounted SystemOS image's `SystemVersion.plist` in `prepare-root-inputs.sh` was run on 24B156's images: its SystemOS
  image has that file, naming 24B156, the same as its OS image. It was not run on 24A446's images, which are expected to
  be the same.

To add a build: put `<BUILD>.conf` beside the others. Its patch list goes under `tools/ios27/s9/`, named
`patches-<BUILD>.txt`. When a root of it has booted, `STATUS=supported`. `DEFAULT` changes in its own commit, when the
project moves.

## Everything that belongs to 24A446

Kinds: **(a)** a tool makes it from the root, so a new build needs only a re-run (the tool and its inputs are named, and
whether the tool itself fixes something of 24A446). **(b)** It was found by hand once, and the finding has to be done again
(how it was found, and a guess of how long). **(c)** It does not depend on the build.

### The build's identity

| Where | What | Kind |
|---|---|---|
| `tools/ios27/builds/DEFAULT`, `24A446.conf` | The build and its names, now in one place. Before this change: `fetch-apple-inputs.sh:16` (`WANT_BUILD=24A446`, `CLASS=n104ap`), `prepare-root-inputs.sh:12,56-58` (the patch list's name and the refusal text), `build-full-root.sh:45-46` (the three images' names, `BUILD_VERSION=24A446`). | a (a new `.conf`) |
| `build-full-root.sh` → herolte tune (`prepare-root-inputs.sh:25`, wardevsa/ios-a5xelte@a43e3e51) | `BUILD_VERSION` becomes `LastSystemVersion` in `com.apple.migration.plist`. If it is wrong, backboardd migrates at the first boot. | a (from the inputs now) |
| `docs/ios27/INSTALL-S9.md:24,47,256`, `fetch-apple-inputs.sh:3,12` | The build named in prose and in the example commands. | c (text) |
| `tools/ios27/mkadt_starlte.py:83-105` | The iPhone 11 identity (`N104AP`, `iPhone12,1`) and its screen. They belong to the device, not to the build. The donor device tree `n104ap.adt` is taken from each restore image by `fetch-apple-inputs.sh`. | a (re-run); a new build's tree may add nodes that `mkadt_starlte.py` does not know: read its output |
| `profiles/ios-27.conf:1,27`, `docs/ios27-starlte.md:12,86-87,103-122` | "iOS 27.0.1's own kernel is xnu-13432.2.10", and the comparison of its system-call table with xnu-12377. | b (repeat for 13432.42.3: half a day, the method is written there) |

### Apple's shared cache: words and addresses

| Where | What | Kind |
|---|---|---|
| `tools/ios27/s9/patches-24A446.txt:7-11` | Five words, each "address, expected word, new word": SystemConfiguration's IOKit-quiesce wait, SafariCore's `WBSSetUpAccessToAppDataContainerWithIdentifier`, QuartzCore's 0.1 s update clamp, PosterFoundation's resource-value count, CoreImage's software context. `cachepatch.py` and `prepare-root-inputs.sh` refuse a word that is not the expected one. | b. Each function is named, and how each was found is in `SPEED.md`, `NIGHT.md`, `TEXT-DATA.md`: find the function by symbol in the new cache and the instruction by its role. Half a day to a day for the five, plus a boot to see each fix still fixes. |
| `tools/ios27/speed/edit-n7.sh:5-11`, `edit-s6.sh:4-10`, `edit-p2.sh:4,22-25` | The same addresses as defaults of the one-off edit tools. | b (they follow the list; or pass the new address as their arguments) |
| `userland/src/sbca.c:61-99` (`SBCA_IOS27`) | QuartzCore of 24A446: 14 unslid addresses (vtables, renderer, the GLES context's constructor and vtable, malloc zone, `function_supported`, the texture shaders' table, the Metal wait's `dispatch_once`, `push_surface`'s branch, the framebuffer format function, the EDR client). Also the struct offsets (`+0x628`, `+0x630`, `+0x60`, `+0x150`, `+0xf0`, `0x480`), the sizes (`GLES_CONTEXT_SIZE 0xba0`, `RENDERER_SIZE 0x68`) and three instruction words (`PUSH_SURFACE_*`, `framebuffer_format_was`). sbca checks every vtable entry and instruction before it changes it, and leaves it alone when it differs (`replace_virtual`, `patch_instruction`, lines 230-277). On another build the result is no GL compositor and a blank screen after the console, not a crash. | b. The hardest single piece: the iOS 26 set was mapped from the iPadOS 18 one with evidence for each (`build/ca-gpu/sbca-ios26-offsets.md`, named at line 103). One to two days, then boots. |
| `tools/ios27/ca-gpu/hakios/hakios.gl.h:39-50` | The order of QuartzCore's 136 texture-function names in 24A446 (`r2e` new at 50), read from `CA::OGL::MetalContext::TextureFunction_codes`. A changed order draws layers with the wrong shader. | b (hours: one table) |
| `tools/ios27/metal2/` (`NativeBridgeMetalFrontend.mm`), `tools/ios27/metal/patches/*.diff` | The Metal device that CoreAnimation talks to. It is Objective-C classes that answer the selectors QuartzCore sends. A new selector in 27.2's QuartzCore is an "unrecognized selector" abort in backboardd. | b (compare the selectors QuartzCore 27.2 sends with what the frontend implements; a day; boots) |
| `tools/ios27/metal/sbmtlc.c:293-307` | Four `os_log` entry points, patched by symbol. The first word is checked (`pacibsp` or `nop`, "measured in 24A446"); with any other word it skips the patch. | c (diagnostic; safe) |
| `tools/ios27/fullroot/pipeline/cache/aot/rewrite-grow.py:523-524` | `SHARED_SIZE = 0x17c228000`, the extent the grow pass may use, an assertion bound (line 565). `HDR_ZERO_FROM = 0x2a8`, the end of the header's used part (an assertion at line 630). Both are facts of 24A446's cache layout. A larger cache or a longer header stops the pipeline with an assertion. It does not make a bad cache. | b (read the two values from the new cache's header; minutes). Making the bound come from the header is a small change, and it should be checked on 24A446's cache before it is relied on. |
| `grow-check.py:10`, `xnu` 0403 (8 GiB shared region) | `SR0`/`SRS`: the fork kernel's shared region, 8 GiB. | c (unless the cache exceeds 8 GiB: not expected) |
| `$IOS27_PRIVATE/undef.txt` and the oracle | `pipeline/cache/aot/make-oracle.sh` over the stock cache (`prepare-root-inputs.sh` step 3). | a |
| `$IOS27_PRIVATE/textdata-reviewed.list` (private), `TEXT-DATA.md:52,85` | Data words inside `__text`. `textdata-guard.py scan` lists the candidates, 145 on 24A446, and a person reviews them. The public build runs without the list (`INSTALL-S9.md:172-174`). | a for the scan, b for the review (half a day to a day) |
| `$IOS27_PRIVATE/coreimage-sw.list` (`steps.d/86-coreimage-sw-context.sh`) | The CoreImage word, for roots built before it joined the patch list. | b (the same word as the patch list's) |
| The a64e, LSE and AOT rewrites (`a64e/a64e-rewrite.py`, `lse/lse-rewrite.py`, `speed/a64e-rewrite2.py`, `pipeline/cache/aot/*`, `dsc-isa-census.py`, `rootrw/`) | They decode every instruction of the cache and of the root's Mach-O files. No address of 24A446 is in them; the counts in `EIGHT-CORES.md:35` and `rootrw/README.md:4` are records. `--skip libcorecrypto` is by name. | a. But an instruction class the new cache uses and 24A446's did not (for example SME, which A13 code should not contain) is not rewritten: run `dsc-isa-census.py` first. |

### The root: names, jobs, settings

| Where | What | Kind |
|---|---|---|
| `tools/ios27/fullroot/lists/` (`daemons-crashloop.list`, `daemons-hw.list`, `daemons-hogs.list`, `daemons-bp.keys` with 122 job paths, `launchd-override-disabled.list`), `tools/ios27/starlte-disabled-ios27.list` | Launchd labels and job paths, chosen from 24A446's boot logs. `edit-launchd.py:44` reports the ones that are not on the root and goes on. | b (the names mostly survive; the crash loops have to be measured again on boots: a day) |
| `check-root.sh:6-9,34-48` | Files and jobs that must be there (`SpringBoard`, `backboardd`, `configd`, `lsd`, `runningboardd` ...), and the cache's main file and `.01`. | c (stable names); a new subcache count is fine |
| `steps.d/50-xpc-disable.sh` (MessagesBlastDoorService), `steps.d/70-lsd-extkit.sh` (FeatureFlag `ExtensionKit/enforce_xpc_cache_code_signing`), `steps.d/40-prefs.sh` (preference keys) | Paths and keys found on 24A446. The steps tolerate an absent path. | b (check that each still exists and still matters: hours) |
| `tools/ios27/speed/edit-wall.sh:3`, `NIGHT.md:117` | PosterBoard's store schema (v61 on 27.0.1). | b (read the new version from the log of the first boot) |
| `stage-full-root.sh` | Copies the three images. No file list. | a |

### The injected libraries (all by name, none by address)

| Library | Source | What it names | Kind |
|---|---|---|---|
| `s.dylib` (a second copy of `sbca.dylib`, backboardd and Preferences) | `userland/src/sbca.c` | The QuartzCore addresses above | b |
| `m.dylib` (wallpaper stand-in) | `tools/ios27/metal/sbwall.m` | PosterBoard and SpringBoard classes | b (check the classes) |
| `b.dylib` (brightness) | no public recipe (`INSTALL-S9.md:108`) | not in the tree | b, for whoever has its source |
| `o.dylib` (rotation, backboardd) | `tools/ios27/sensors/orientation.c:87-206` | `CMDeviceOrientation`, `CMDeviceOrientationManager`; `initWithOrientation:andTimestamp:`; methods replaced by name | b (CoreMotion 3185 → 3186 in 27.2: check the names and the method signatures, an hour) |
| `c.dylib` (status bar, SpringBoard) | `tools/ios27/modem/scion-bars.m:84-329` | `STStatusBarData`, `STMutableStatusBarData`, `STStatusBarDataCellularEntry`, `SBStatusBarStateAggregator`, `STStatusBarOverridesStatusDomainPublisher`, `STStatusBarDataCellularTypeDescription` | b (StatusKit 154.100 → 154.200 in 27.2: an hour) |
| The kernel's built-in line (`xnu` `osfmk/arm/arm_init.c:190`) | `sweetinject=` with s, m, b | process names `backboardd`, `Preferences`, `PosterBoard`, `SpringBoard` | c |

### The kernel's answers to what iOS 27 asks (the `xnu` fork)

| Where (xnu fork) | What | Kind |
|---|---|---|
| commit 0120, `bsd/kern/sys_pipe.c:400`, `kern_descrip.c:2537`, `vfs_syscalls.c:7990,8909,11335,13855`, `kern_aio.c:3478` | Darwin 27's eight system calls, found by comparing 27.0.1's kernelcache with xnu-12377 (`docs/ios27-starlte.md:103-122`) | b (repeat the comparison for 13432.42.3) |
| `bsd/kern/lockdown_mode.c:43` | `security.mac.lockdown_mode_state_public`: libsystem_darwin aborts without it | b (sysctl names that 27.2's libraries read) |
| `bsd/kern/sweet_amfi.c:292-312,575-594` | Sandbox extension argument layouts "as libsystem_sandbox of iOS 27.0.1 passes them". AMFI sysctls `developer_mode_status`, `developer_mode_resolved`, `hw.features.allows_security_research`. | b |
| `iokit/Kernel/arm/SweetKeyStore.cpp:24-40`, `KEYSTORE.md` | AppleKeyStore selectors and answers ("24A446: the selector numbers and argument shapes ...") | b (AppleKeyStore 2383.2.1 → 2383.40.14 adds key-state fields: check the state's consumers) |
| `bsd/sweet_hfs/sweet_hfs_pclass.h:21-27` | APFS purgeable `fsctl` numbers "as iOS 27.0.1's PosterFoundation makes them" | b |
| `drivers/wifi/hakios.apple80211.c:17` (scion-kernel, `HAKIOS_APPLE80211_27`) | IO80211's request and event layouts of 27.0.1 | b |
| The other `sweetspeed` bits, the AMFI stand-in, IOSurface and IOMFB stand-ins | Answers seen to be needed on 24A446 | c until a boot says otherwise |

The boot command line (`tools/ios27/cmdline/*.txt`, `sbgl=0 sweetspeed=408782847`) names nothing of the build. It was
proven only with 24A446, which is what `INSTALL-S9.md:256` means.

## What is likely to be different in 27.2.1 (public sources only)

**S** = sourced (the source is named), **E** = my expectation.

- **S** For the iPhone 11 there was no 27.1. The builds were 27.0 (24A437), 27.0.1 (24A446), 27.2 (24B119, 26 October,
  no longer signed) and 27.2.1 (24B156, 9 November, signed). The 27.2.1 restore image is 10,977,415,204 bytes, 270 MB
  more than 24A446's 10,706,900,826. (api.ipsw.me, device iPhone12,1; 27.1 is listed only as build 24A94425 for one
  device.) 27.3 is in beta (24C5037i, 24C5049d).
- **S** Apple's kernel: 27.0 and 27.0.1 `13432.2.10~2` (Darwin 27.0.0); 27.2 betas `13432.40.144` → `.40.162` →
  `.40.177` → `13432.42.3~13`; 27.2 and 27.2.1 `13432.42.3~28` (Darwin 27.2.0), the same build in both
  (blacktop/ipsw-diffs, iOS 27_0_24A437_vs_27_0_1_24A446 to 27_2_24B115_vs_27_2_1_24B156; those diffs are of the
  iPhone 19 images). **E** The iPhone 11's kernel is the same version.
- **S** Three new kernel extensions in 27.2: `com.apple.driver.ApplePencilDriver`, `com.apple.iokit.EndpointSecuritySE`,
  `com.apple.kec.AppleEncryptedArchive`; `com.apple.iokit.AppleSecurityResearch` in beta 3 (ipsw-diffs). Endpoint
  Security on iOS starts only on security research devices. On a retail phone the kext does nothing and publishes no
  service; `watchdogd` is the only holder of the client entitlement (calif.io, "Endpoint Security ships in iOS 27.2 beta",
  18 Sep 2026). **E** User space already copes with its absence, as on every retail iPhone. Scion's kernel loads none
  of Apple's kexts; `hw.features.allows_security_research` already answers 0.
- **S** `libsystem_kernel` 13432.2.10 → 13432.40.144 exports two new symbols, `_os_sme_engine_attr_count` and
  `_os_sme_engine_attr_read`, and grows by 188 bytes of code. It did not change again through 24B5110a (ipsw-diffs). **E**
  No new system-call wrapper is exported, so a new system call is unlikely. The two SME functions are for cores with SME,
  which A13 code should not call.
- **S** Precedent from Apple's published XNU: the x.0 → x.2 kernels (`xnu-10002.1.13` → `.41.9`, `11215.1.10` →
  `.41.3`, `12377.1.9` → `.41.6`) changed no system-call number and no Mach trap. Later point releases changed the
  arguments of one trap each (`exclaves_ctl_trap` in 11215.81.4, `mach_vm_reclaim_update_kernel_accounting_trap` in
  12377.121.6). (`bsd/kern/syscalls.master` and `osfmk/kern/syscall_sw.c` at those tags.) **E** The same for 13432.42.
  Changes inside MIG routines, new sysctls and new IOKit properties are the risk; the tables are not.
- **S** dyld 27062 → 27104 (ipsw-diffs). **E** The shared cache's header format is the same. Not known; the pipeline's
  assertions catch a change.
- **S** Frameworks Scion touches all changed in the first 27.2 beta: QuartzCore 1223.0.18 → 1223.10.10 (+31 KB of code,
  +55 functions, new `CAContext` resize-anchor methods); CoreImage, SystemConfiguration, SafariCore, CoreMotion
  (3185 → 3186), StatusKit (154.100 → 154.200), AppleKeyStore (2383.2.1 → 2383.40.14, ten new `der_key_state_abs_*`
  symbols: last passcode and "mesa" (Touch ID) unlock times, lock time), MobileKeyBag, IOMobileFramebuffer, IOGPU,
  PosterBoard, libsandbox (3051.0.52 → 3051.40.70), libxpc, libMobileGestalt, libcorecrypto (ipsw-diffs, Dylibs.Updated).
  `libsystem_sandbox` and `libsystem_darwin` are not in that beta's list.
- **S** New programs in the first 27.2 beta (iPhone 19 images): `/usr/libexec/memoryanalyticsd`, `/usr/libexec/appledepthd`,
  `healthcontentd`, `speechmodeltrainingd` (moved from CoreSpeech to CoreEmbeddedSpeechRecognition), 26 new
  ExtensionKit extensions (app replacement and migration); `aggregated` is removed (ipsw-diffs). **E** On the iPhone 11
  image some of these may be absent (`appledepthd` is for depth hardware). The launchd lists need a boot to settle.
- **S** The new shared caches for the iPhone 19 images gained subcaches (`.75` to `.79`). **E** The iPhone 11's cache count
  changes too. The tools glob and do not count.
- **S** Security: 27.2 fixes about 15 kernel bugs (use-after-free, a copy-on-write integrity issue, races), IOSurface,
  IOMobileFrameBuffer, IOKit and IOStorageFamily bugs, and Sandbox Profiles (Apple, "About the security content of iOS 27.2",
  support.apple.com/en-us/149230). 27.2.1 has no kernel entry; it fixes AirPlay, App Intents, Bluetooth, brotli,
  CoreUtils and Messages (149715). **E** Scion's kernel is xnu-12377 plus its own code, so none of Apple's kernel fixes
  reach the S9 by moving. What moves is the user-space fixes.

## 24B156: what was found (17 November 2026, from the restore image)

The owner's own restore image (`iPhone12,1_27.2.1_24B156_Restore.ipsw`, 10,977,415,204 bytes) was on the Mac mini.
`IOS27_BUILD=24B156 tools/ios27/s9/fetch-apple-inputs.sh` took its inputs into an agent's folder: OS 9.3 GB, SystemOS
6.2 GB, AppOS 16 MB, the device tree 235 KB. The images were read mounted read-only. Nothing of them is in the
repository; the numbers in `patches-24B156.txt` and `24B156-sbca.txt` are addresses and instruction words, as for 24A446.
Not run on a phone; no root was built.

**Kernel** (the kernelcache `kernelcache.release.iphone12b`, read with blacktop's `ipsw kernel`; compared with the xnu
fork's `syscalls.master` and `syscall_sw.c`, and with the MIG subsystems of the fork's own built STARLTE kernel, by
symbol):

- Version: `Darwin Kernel Version 27.2.0 ... root:xnu-13432.42.3~28/RELEASE_ARM64_T8030`.
- System calls: 558 entries (0 to 557). No number that 24B156 implements is missing or different in the fork, and the
  argument counts agree. Darwin 27's eight calls are where commit 0120 put them. The fork additionally implements 18
  numbers that this iOS kernel compiles out (System V IPC, `grab_pgo_data`, ...), as for 27.0.1.
- Mach traps: 62 real traps, the same numbers, names and argument counts as the fork's table. None was added or moved.
- MIG: the same 17 subsystems at the same bases. Two of them end one routine later than the fork's:
  - `is_iokit` 2800-2892 (fork 2800-2891). Routine 2891 is `io_service_get_wait_info`, the request of IOKit's new
    `IOServiceGetWaitInfo` (its client stub sends id 2891 and expects 2991).
  - `memory_entry` 4900-4905 (fork 4900-4904). Routine 4904 is `mach_memory_entry_region_info` in `libsystem_kernel`.

  Scion's kernel answers both with `MIG_BAD_ID`, so the caller gets an error and no crash. Who calls them was not found:
  not SystemConfiguration, `configd`, `backboardd` or `launchd`. A daemon that cannot live with that error is the one
  kernel-side risk found. The fix, when one shows, is a routine in the fork's `device.defs` / `memory_entry.defs`.
- The sysctl names the stand-ins answer (`security.mac.lockdown_mode_state_public`, AMFI's `developer_mode_status` and
  `developer_mode_resolved`) are still in the cache. The four APFS `fsctl` numbers of `sweet_hfs_pclass.h` are still in
  PosterFoundation.

**Shared cache** (`System/Library/Caches/com.apple.dyld` of SystemOS):

- Format: magic `dyld_v1  arm64e`; the main header's mapping array at 0x238, as in 24A446.
- Size: 80 files with headers (main and `.01` to `.79`; the last three are `.dylddata`, `.dyldreadonly` and
  `.dyldlinkedit`), plus `.atlas` and `.symbols`. 24A446 had 79.
- `sharedRegionSize` is 0x187948000 (the mapped extent 0x187944000). That is larger than `rewrite-grow.py`'s
  `SHARED_SIZE` 0x17c228000, but none of the 34 gaps the grow pass may use ends beyond it (0xf668000 bytes of room in
  all). So the assertion holds and the constant need not change for 24B156.
- Every gap owner has one RX mapping, the code signature at the end, and a zero header tail from 0x2a8. These are the
  assumptions of `rewrite-grow.py`'s `finalize`.
- Within the fork's 8 GiB shared region.
- Not run (no numpy on the Mac mini): `dsc-isa-census.py`, the cache pipeline, `make-oracle.sh`, `textdata-guard.py
  scan`. These are the first things to run on 24B156, in that order.

**The five patch words.** Each was found by the function its 24A446 line names. `prepare-root-inputs.sh` (run as far as
its numpy step) found every expected word, and `cachepatch.py --dry-run` would patch each one:

| 24A446 line | 24B156 | Confidence |
|---|---|---|
| SystemConfiguration, the IOKit-quiesce block | `____wait_for_IOKit_to_quiesce_block_invoke`, first word `pacibsp` → `ret` | high: the same symbol, the same word |
| SafariCore `WBSSetUpAccessToAppDataContainerWithIdentifier` | its first word, `pacibsp` → `ret` | high (assumes it is still a void function) |
| QuartzCore `render_for_time` clamp | the `tbz w20, #0` that skips the now + 0.1 s clamp unless the display is internal and on (bytes 1 and 5 of its state), now `+0x50` (was `+0x54`): → `b +0x50` | high: the same test and the same constant after it |
| PosterFoundation viability dictionary | `PFPosterPathURLResourceValues`'s block: `NSDictionary` of IsReadable and FileProtection(None), `mov w4, #2` → `#1` | high |
| CoreImage default context | **not carried over.** `-[CIContext initWithOptions:]` is laid out differently. The 24A446 word was a `bl` to an Objective-C stub, turned into `b +0x38`; no such call in the same position. | — |

**sbca, the texture table, the injected libraries:**

- All 14 QuartzCore addresses exist by name (`24B156-sbca.txt`).
- The words sbca checks before it patches are the same: `PUSH_SURFACE_CBNZ` and the three words of
  `framebuffer_format_was`.
- The renderer slot (+0x1d0) and the `function_supported` slot (+0x480) are unchanged.
- The server's +0x628/+0x630/+0x60, the 0x68-byte renderer and the +0xf0 load are in the new `renderer()`.
- Not checked: `GLES_CONTEXT_SIZE` and the display's +0x140/+0x150.
- `TextureFunction_codes` has the same 136 names in the same order as `hakios.gl.h`.
- Rotation (`orientation.c`): `CMDeviceOrientation`, `CMDeviceOrientationManager` and the five selectors exist.
- Status bar (`scion-bars.m`): `STStatusBarData`, `STMutableStatusBarData`, `STStatusBarDataCellularEntry`,
  `STStatusBarOverridesStatusDomainPublisher` (`updateDataWithBlock:` on its superclass), `cellularEntry`,
  `entryForKey:`, `setCellularEntry:` and `STStatusBarDataCellularTypeDescription` exist. `SBStatusBarStateAggregator`
  exists but has no `+sharedInstance` in 24B156; the library asks `respondsToSelector:` and skips that path. Whether
  24A446 had it is not known.
- `sbwall.m` uses public UIKit only.

**Device tree.** `mkadt_starlte.py --iphone11` and `adt-extra-ram.py` ran on 24B156's `n104ap.adt`. The donor gives
the `/product` node, which is there.

**Root names:**

- Every label and job key of `fullroot/lists/` is in 24B156's `launchd.plist`, except `com.apple.datamigrator` (an
  override; harmless when absent).
- `starlte-disabled-ios27.list` matches fully; `starlte-disabled.list` lacks only `com.apple.cloudd.sync`.
- New jobs on this image include `com.apple.appledepthd` (depth hardware the S9 does not have: a likely crash loop, a
  candidate for `daemons-hw.list`), `healthcontentd` and `speechmodeltrainingd`. `memoryanalyticsd` is not on the iPhone
  11 image. `aggregated` is gone.
- `MessagesBlastDoorService.xpc` and the ExtensionKit flag `enforce_xpc_cache_code_signing` are there.

**Not carried over mechanically**, to do before 24B156 can replace 24A446:

1. The CoreImage word: by hand, then a phone.
2. sbca's 24B156 block in `sbca.c`, chosen at build time: from `24B156-sbca.txt`, plus `GLES_CONTEXT_SIZE` and the display
   offsets checked.
3. The selectors 27.2's QuartzCore sends to the Metal device, against `tools/ios27/metal2/`.
4. The cache pipeline on 24B156 (`dsc-isa-census.py` first), the oracle, the text-data scan and review. These need numpy.
5. On the first boots:
   - whether anything needs `io_service_get_wait_info` or `mach_memory_entry_region_info`;
   - PosterBoard's store schema;
   - AppleKeyStore's new key-state fields;
   - the sandbox-extension layouts;
   - IO80211's layouts;
   - `appledepthd` and the crash loops.
6. `b.dylib` and the Metal bundle rebuilt for 24B156 if they link anything that moved.

**What 24B156 needs before it can boot, with confidence:**

- **Nothing in the kernel is known to be missing** (high confidence for system calls and traps; medium for the two MIG
  routines, which return an error rather than stop the boot).
- A root built with the four-word patch list and `sbca`'s 24B156 block should reach SpringBoard as 24A446's did (medium:
  the rewrite pipeline has not run on this cache).
- Without the sbca block it boots to a blank screen, and without the CoreImage word some Settings symbols are missing.

## What only the new restore image can answer, in the order to ask

(Written before the image was on the disk. Questions 1 to 3 and most of 5 to 7 are answered in the section above. 4 is
answered for the system calls, traps, sysctl names and fsctl numbers, and 8 is not.)

1. **The kernel's tables, before the 11 GB download.** Take only the kernelcache and the manifest out of Apple's
   remote restore image. blacktop's `ipsw` reads single files of a remote IPSW. The kernelcache is a few tens of MB:
   ask Claudius first, since it is Apple's file and any download is his decision. Compare as `docs/ios27-starlte.md:103-122`
   did:
   - the system-call table: 557 entries and the eight Darwin 27 calls;
   - the Mach traps' argument counts;
   - the MIG subsystems and their ranges;
   - the sysctl names that the AMFI and sandbox stand-ins and `lockdown_mode.c` answer.

   A new call or trap here is the one finding that could stop the boot early. It is also the cheapest to learn.
2. **The manifest:** the `n104ap` Erase identity and the three images' names, for `24B156.conf`.
3. **The cache's header:** format, mapping count, the extent against `SHARED_SIZE` (`rewrite-grow.py:523`), the header's
   used size against `HDR_ZERO_FROM`, and the subcache count. Then `dsc-isa-census.py`, to see whether any instruction
   class appears that the rewrites do not cover.
4. **The user side of the kernel:**
   - `libsystem_kernel`'s system-call stubs: every `svc` number, to confirm step 1;
   - `libsystem_darwin`'s sysctl names;
   - `libsystem_sandbox`'s `sandbox_extension_*` layouts;
   - AppleKeyStore's `aks_*` selector numbers (`KEYSTORE.md`'s table);
   - PosterFoundation's `fsctl` numbers;
   - IO80211's request layouts.
5. **The five patch words** (`patches-24A446.txt`), by the functions they name. This gives `patches-24B156.txt`.
6. **QuartzCore for `sbca`:** the 14 addresses, the offsets, the two sizes and the three instruction words. Then the
   texture-function table (`hakios.gl.h`), then the Metal selectors QuartzCore sends.
7. **Names the libraries and steps use:** CoreMotion and StatusKit classes and selectors; PosterBoard's store schema;
   the ExtensionKit feature flag; MessagesBlastDoorService's path; every label in `fullroot/lists/` and
   `starlte-disabled-ios27.list`.
8. **`textdata-guard.py scan`:** the data-word candidates to review.

## The steps, in order

| # | Step | Who | Disk on the Mac mini |
|---|---|---|---|
| 0 | Free disk (below). Decide which test phone carries 24B156. | Claudius; Dylan for the phone | — |
| 1 | **Done 17 Nov.** Question 1 above: the kernelcache and the manifest only. If a new call or trap shows up: an xnu fork commit (APSL, like 0120), STARLTE and FRANKEL built. | an agent (download approved by Claudius) | < 200 MB |
| 2 | **Done 17 Nov** (into an agent's folder, from the owner's download). The restore image from Apple, then `IOS27_BUILD=24B156 tools/ios27/s9/fetch-apple-inputs.sh IPSW inputs-24B156`; delete the IPSW. | an agent | 11 GB for the image, then about 25 GB free during the unpacking (the script's own figure); about 20 GB stays (three images and the tree) |
| 3 | **Partly done 17 Nov** (`patches-24B156.txt` without CoreImage, `24B156-sbca.txt`). Questions 2 to 8 with the images mounted read-only. Write `patches-24B156.txt`, `sbca.c`'s `SBCA_IOS27` set for 24B156 (a second block chosen at build time, so 24A446 keeps its own), the texture table, the conf's image names. Draft PR, `STATUS` still `candidate`. | an agent; no phone | none beyond step 2 |
| 4 | Userland for 24B156: `sbca`, `libsbgl`, the Metal bundle if needed, o/c/m dylibs. The overlay directory. | an agent | ~10 GB build tree (shared with 24A446's) |
| 5 | `IOS27_BUILD=24B156 prepare-root-inputs.sh` into a SEPARATE private directory (`IOS27_PRIVATE=.../private-24B156`), then `sudo IOS27_PRIVATE=... tools/ios27/s9/build-root.sh inputs-24B156`, `OUT=.../root-24B156.img`. | an agent with `sudo` (or Claudius) | 62 GB free when it starts (`build-full-root.sh:60-61`); about 20 GB stays (the image); delete `work/` after |
| 6 | `make-boot-image.sh` with the same kernel and the new root's `.inject` record. | an agent | < 100 MB |
| 7 | Flash the root to the 24B156 phone's USERDATA, boot, read the USB console. Iterate on what step 1 and the first boots show. | Dylan, or an agent with the phone when that is allowed | — |
| 8 | Parity with 24A446, feature by feature: picture and frame rate, touch, Settings icons, rotation, Wi-Fi, modem and bars, audio and Bluetooth, cameras, Control Centre. Then the launchd lists again. | agents with the phone | — |
| 9 | When 24B156 is at least as good: `STATUS=supported`, `DEFAULT` → `24B156` (its own commit), `INSTALL-S9.md` updated. Keep `24A446.conf` and its patch list as long as a phone runs it. | Claudius merges | delete 24A446's inputs only when no phone or agent needs them |

**Keeping 24A446 working beside it.** Everything per build is separate in the tree, so both can live there:

- `24A446.conf` and `24B156.conf`;
- `patches-24A446.txt` and `patches-24B156.txt`;
- the two address sets in `sbca.c`, chosen at build time.

On the Mac mini, give each build its own inputs directory, private directory (`IOS27_PRIVATE`), root image (`OUT`) and
`work/`. The checks refuse a mixture of any of them. The kernel is one kernel for both, unless step 1 finds something; a
kernel change for 24B156 must keep 24A446 booting, and both are tested.

**The phone holds one root.** USERDATA holds one root image (the root is written at byte 0 of USERDATA), so a phone runs
one build at a time:

- Switching builds is a full root flash (about half an hour with `fastflash-root.py`) plus its boot image, and that
  phone's measurements on 24A446 stop.
- Use a second S9 for 24B156 if there is one. Otherwise take the phone at an agreed time, after the rendering and
  performance agents have what they need.
- Keep the 24A446 root image and boot image on the Mac mini (about 20 GB) until the move is done, so that the phone can
  be put back the same day.
- A root on the microSD card (`docs/ios27/XNU-MICROSD.md`, if that path is far enough along) would let one phone carry
  both. That is not assumed here.

## Disk

On 17 November the Mac mini had 42 GB free (`df -h /System/Volumes/Data`, 96 % used) with seven agents working.

| Holding at once | GB |
|---|---|
| 24A446: its root image and boot image (kept to go back) | ~20 |
| 24B156: the three unpacked images | ~20 |
| 24B156: the restore image while it is unpacked (then deleted) | 11, plus ~14 working room |
| 24B156: the root build (`MIN_FREE_GB=62` when it starts: the copy, the cache, room) | 62 free, ~20 kept |

**Peak:** about 100 GB free at the root build, with 24A446's root kept and its unpacked inputs deleted. Keeping
24A446's inputs too adds about 20 GB.

**Without the peak:** steps 1, 3 and 4 need only the 24B156 inputs (about 40 GB at their own peak, 20 GB after). Step 3
can be done with the images unpacked, before anyone has room for a root build. An external disk would remove the
constraint. None of this fits beside seven agents' build trees today.

## Risks

- **A kernel interface that xnu-12377 lacks.** This is how it would show, from how 27.0.1 went:
  - A missing system call kills the caller with SIGSYS. Darwin 27's `aio_readv`/`aio_writev` answer ENOTSUP for this
    reason (`ios27-starlte.md:118-122`).
  - A missing sysctl that a library requires aborts every process that links it. On 27.0.1 libsystem_darwin aborted
    without `security.mac.lockdown_mode_state_public` (`xnu` `bsd/kern/lockdown_mode.c:43`). When launchd or its first
    jobs die like that, the console stops early, before backboardd, and the kernel log shows the process's exit and
    signal. The fork's per-process exit and signal lines are in `NIGHT.md:22`.
  - A changed MIG routine or IOKit selector shows as one daemon failing or looping. It does not stop the boot.

  For 27.0.1 the calls were added before the first boot, from a comparison of the kernelcache (commit 0120). The
  sysctls and stand-in answers were added as each boot showed the next stop (`IOS27-TARGET.md:60-83`). Do the same:
  step 1 first, then boots with the console attached.
- **The picture.** `sbca` leaves every hook it cannot verify alone. On new addresses that means no compositor, a phone
  that boots to a blank screen, and nothing in the log beyond sbca's "found ..., expected ...; left alone" lines. The
  Metal path can abort backboardd on an unknown selector. Most of the bring-up time is here.
- **The rewrites.** A cache that is larger, or a header that is longer, stops `rewrite-grow.py` on an assertion
  (`:565`, `:630`). A new instruction class that is not rewritten traps on the M3 cores at run time; the kernel emulates
  only what it knows. Run `dsc-isa-census.py` before the first root.
- **Losing the measurements.** Any number measured on 24A446 (frame times, the patch words' effects, crash loops) has to
  be measured again on 24B156. A move in the middle of the open rendering and performance work would throw their
  baselines away.
- **Moving again soon.** 27.3 is in beta. Each move costs most of the steps above again. The manifests make the
  mechanical part a file. The finding by hand stays.

## Effort and when

| Part | Agent time | Phone time |
|---|---|---|
| Step 1 | done (the section above) | — |
| Step 3: the CoreImage word, sbca's 24B156 block in code, the Metal selectors (four words, the addresses, the table and the names are done) | 1 to 2 days | — |
| The cache pipeline, oracle and text-data scan on 24B156 (needs numpy) | half a day | — |
| Step 4 and 5 | 1 day | — |
| First boots to a picture | 1 to 3 days | yes |
| Parity, feature by feature, and the launchd lists | 3 to 5 days | yes |
| A kernel change, if step 1 finds one | + 1 to 3 days | yes |
| **Total** | **6 to 12 working days** | **4 to 7 of them** |

**Recommendation.** Do not move while the rendering, performance, camera and Control Centre work is being measured on
24A446. Their findings are addresses and baselines of 24A446, and moving first would mean finding them twice. Order:

1. Done on 17 November: step 1, steps 2 and 3 as far as tools and names go (the section "24B156: what was found").
2. Offline, in parallel with that work: the rest of step 3 (the CoreImage word, sbca's 24B156 block, the Metal selectors) and the cache pipeline on 24B156 (needs numpy).
3. Once those tracks have landed: the phone steps, on whichever build is then the newest. The work is the same for
   27.2.1 or 27.3.

Until then the user space stays 24A446, which runs and is measured. The kernel fixes in Apple's 27.2 do not apply to the
S9's kernel either way.
