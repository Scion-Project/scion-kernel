# Upstream sync, 22 November 2026

On 22 November 2026 HakiOS (oosexgang) announced a large update: a reworked codebase and documentation, and a new UEFI
based on U-Boot with dual boot. This page records what of it Scion took, what it left out and why, and what waits for a
decision. It covers the three repositories Scion forks from HakiOS. The one line per commit for the kernel repository is in
[upstream-sync-2026-11-22-commits.md](upstream-sync-2026-11-22-commits.md). The last sync was 18 November
([UPSTREAM-SYNC-2026-11-18.md](UPSTREAM-SYNC-2026-11-18.md)).

## What changed upstream

- **hakios-xnu-kernel**: `main` has not moved since the merge base Scion took on 6 November (`63fc2b4633`). The work is
  on a new default branch, `common` (head `62e09f85`, 22 Nov), which shares **no history** with `main` or with Scion. It
  grew out of the Renoir (SM7350) XNU 12377 release (`f2cce861`, 10 Nov) and hakios-metal's history, and from 11 November
  it was reorganised along the lines of Linux's tree (`arch/arm64/boot` for the loader, `drivers/`, `kernel/overlay` for
  XNU's sources, `components/`, `init/`, `lib/`, `workbench/`, `Documentation/`). It has 2387 commits: 1954 by shimoku74,
  400 by vcvkk, 19 by hxfuxyy and 14 by wannq.
- **hakios-uefi**: 7 commits. The Mu-Silicium boot path is replaced by U-Boot built from board profiles, with an SD-card
  (FAT) boot path and a porting guide.
- **hakios-sm7325**: 7 commits. The device package is renamed (`redwood` -> `poco-x5-pro` -> `sm7325-device`), and one
  commit adds the phone's vendor firmware.

## How it was read

The previous syncs merged upstream's `main` (`a19cdf6d`, 8 Nov; `775d0f80`, 31 Oct). With no common history that is not
possible here, and a tree merge would bring a second, differently laid-out kernel tree into this one. So the method is the
one used for UniXNU's rewritten history on 18 November: compare file contents, then port one commit at a time.

1. Files that exist in both trees were matched by name (640, mostly `components/metal` = `hakios-metal/` and
   `userland/userland/src` = `userland/src`). For each one, the newest upstream version equal to a version in Scion's
   history, or in upstream `main`'s, is its sync point. 275 are identical now. 144 changed after a version Scion has. 216
   never matched: upstream's copy descends from Renoir's line, not from the copy Scion forked.
2. The upstream commits since 14 November that touch those files were read by their subjects (191). The fixes to shared
   code among them (72) were ported with a path mapping (`init/` -> `userland/src/`,
   `kernel/lx/` -> `wifi/lx/`, `arch/arm64/boot/` -> `xnuloader/`, `components/metal/plugin/` -> `hakios-metal/`, ...)
   and `git am -3`. Each commit keeps its author, and its message says which upstream commit it is and which of its files
   Scion does not have. Nothing was squashed.
3. Every other commit was put in a group by the files it touches, by script. Then the groups were checked by hand against
   Scion's rules: nothing of Apple's, no vendor blobs, no SEP, key, attestation or DRM work, and brcmfmac/LinuxKPI only for
   Wi-Fi.

## Licence position (verified from the files)

- **hakios-xnu-kernel `common`**: no `LICENSE` file at the top, which is why GitHub detects none. `NOTICE.md` sets the
  licences by directory, and `LICENSES/` holds the APSL-2.0 and MIT texts:
  - the kernel overlay and patches: APSL-2.0;
  - **`userland/`, `arch/arm64/boot/` (the loader, which was `xnuloader/`) and `kernel/deps/`: "HakiOS's licence
    (`LICENSES/other/APSL-2.0`)"**;
  - `tools/`, `workbench/`, `bench`, the DTS and the documentation: MIT;
  - `components/metal/plugin/` (hakios-metal): its own `LICENSE`, MIT, "Copyright (c) 2025-2026 the HakiOS authors";
  - `kernel/lx/` (the Wi-Fi layer): not named.

  The files have no SPDX headers (checked: `init/*.c`, `userland/userland/src/*.c`, `kernel/lx/src/*.c`,
  `arch/arm64/boot/src/*.c`, `tools/apfs/apfs-tool.c`).
- **The conflict.** Upstream `main`'s `LICENSE`, which Scion's `LICENSE` keeps, licensed "the boot loader in xnuloader/,
  the user-space code in userland/src/, the tools ... MIT". `common`'s NOTICE.md now says that user space and the loader
  keep HakiOS's licence, *APSL-2.0*. Upstream's changes to `userland/src`, `xnuloader/` and (unnamed) `wifi/lx/` therefore
  come under a licence that differs from, or is not stated for, the Scion files they would change. **They are not merged**
  until the owner decides, or upstream clarifies. They wait on their own branch.
- **Clear**: hakios-metal (MIT, the same `LICENSE` as Scion's `hakios-metal/`) and `tools/apfs` (MIT under NOTICE.md's
  `tools/` line, as Scion's tools are). Those are taken.
- **hakios-uefi** and **hakios-sm7325**: no `LICENSE`, no NOTICE. Only some files have headers: U-Boot's C files are
  `GPL-2.0+`, a redwood DTS is `BSD-3-Clause`, and the EDK2-derived platform files are `BSD-2-Clause-Patent`. Nothing
  covers the scripts, configs, env files or docs, so neither pull request is merged.

## Decisions: hakios-xnu-kernel `common`

| Decision | Reason | Commits |
|---|---|---:|
| taken (PR: hakios-metal, tools/apfs) | ported (PR, mergeable) | 7 |
| ported, waiting (PR: licence, S9 test) | ported (PR waiting: licence and S9 phone test) | 23 |
| waiting | conflicts with Scion's diverged copy: port after the licence decision | 28 |
| waiting | needs a decision: a var_store privacy fix (the keychain kept out of the saved /private/var) that also changes the keystore stand-in, which is SEP territory | 1 |
| left out | Metal (hakios-metal, the Adreno driver) development: four of its fixes are taken (first group) and three wait (conflicts); the rest is left, as in the 18 Nov sync | 193 |
| left out | Apple SEP / keystore / AMFI / FairPlay / App Store / Apple ID / IPSW decryption work: excluded by Scion's rules | 64 |
| left out | screenshots of Apple's user interface (docs/*.png, *.jpg): never in Scion repositories | 3 |
| left out | also changes the FairPlay daemon's launchd job (fairplayd restarts): kept out with the FairPlay work; its dasd part conflicts anyway | 1 |
| not applicable | build system, test bench and tools of the new tree's layout | 580 |
| not applicable | documentation of the new tree's layout | 542 |
| not applicable | user space in the new tree's layout (renames and Qualcomm-board work): not read one by one before 14 Nov; see the shared-file list | 363 |
| not applicable | drivers and board data of the new tree (Qualcomm/MediaTek boards; Renoir, Redwood and others): no counterpart in Scion's tree | 250 |
| not applicable | XNU source of the new tree: Scion's kernel source is the fork Scion-Project/xnu, not this repository | 232 |
| not applicable | other files of the new tree | 81 |
| not applicable | touches only files Scion has not got (new tree, kernel source or their test bench) | 11 |
| not applicable | XNU source of the new tree (IOKit stand-ins): Scion's kernel source is the fork Scion-Project/xnu | 1 |

### Taken (this pull request; cannot reach the Galaxy S9's build)

hakios-metal is the Metal driver for the Adreno GPU, linked into the 16.5 plug-in only when it has been built by hand (`make
ios`, `userland/build.sh`); the S9's Mali never runs it. `tools/apfs` is a host tool. Host tests: hakios-metal's 263 tests
pass (261 before; built with `c++ -std=c++17 -Wall -Wextra -Werror`, `-Wno-error=unused-function` for a warning `main` has
too). The kernel half of `d7d60957` (`fs/apfs/apfs_lib.c`, the census in the kernel) belongs to the fork
Scion-Project/xnu and is not in this repository.

| Upstream | Date | Subject |
|---|---|---|
| `4141e978` | 2026-11-16 | tools/apfs: an unused import, a sum that is always 0, an unused size |
| `81d9794f` | 2026-11-16 | tools/apfs/bench-qemu: the wait loop's counter is not used |
| `23429141` | 2026-11-18 | metal: an indirect draw's, an indirect dispatch's and a visibility result's offset bounded by subtraction, so a wrapping offset is refused, ... |
| `5d912e88` | 2026-11-18 | metal: an argument buffer member's offset bounded by subtraction too, so set.offset + member offset cannot wrap past the buffer and pass |
| `b38d5c99` | 2026-11-18 | metal: a deferred command buffer's job counter is shared, so a buffer freed inside its own completion handler on the deferrer's worker is no... |
| `d7d60957` | 2026-11-18 | apfs: a directory record whose name is empty or has a zero byte or a '/' in it is damage (the census walked into the directory it is in, wit... |
| `ea329fcb` | 2026-11-18 | metal: a wait that times out with the GPU neither finishing nor reset quarantines the command buffer's memory instead of returning its comma... |

### Ported, waiting (a separate pull request: licence decision first, then a phone test on the S9)

These change user space (`init.c`, `var_store.c`, `inject_*.c`, `eagl.c`, the camera device), the loader and the Wi-Fi
layer. `wifi/lx/src/lx_base.c` is linked into the S9's brcmfmac; `lx_bus.c` and `lx_80211.c` are ath11k's (the POCO's).

| Upstream | Date | Subject |
|---|---|---|
| `b2c01c92` | 2026-11-17 | wifi: an earlier kernel's DSP shut down without stopping this kernel's driver, which closed the radio's registers before its start |
| `d0163e78` | 2026-11-17 | lx: the rings' thread stopped and the registers closed when the driver's start gives up |
| `07153a4a` | 2026-11-18 | camera: a photograph counted as given only when a buffer took it |
| `341c9eaf` | 2026-11-18 | init: a long root_apfs= device, a long name in a walked tree and a long var.skel line refused, not copied over the stack |
| `3df45a05` | 2026-11-18 | init: background jobs that have ended taken back before each command and while a list sleeps, not left as zombies |
| `4ce94d0d` | 2026-11-18 | wifi: a match for the radio's stream left by an earlier kernel is taken over and sent to fault, not matched twice; a DSP that gives up or a ... |
| `54f5cbef` | 2026-11-18 | eagl: a texture from an IOSurface no bigger than the surface or its plane, and 32-bit pixel types read as such |
| `5cb4e74e` | 2026-11-18 | var_store: a directory with more names than the store holds fails the save, not saved as nothing over the good slot |
| `5e5e12f9` | 2026-11-18 | lx: delayed work queued by the ticker before it lets go of the lock, and timer_delete_sync() waits for a timer that fires |
| `70533e91` | 2026-11-18 | inject_app: the spawn log held to its line however long the arguments |
| `741b7010` | 2026-11-18 | camera: auto-exposure's gain held to the sensor's most, so the frames' gain is again what was asked |
| `7aecc355` | 2026-11-18 | camera: a stream finalized taken out of the streams' list the other one looks at |
| `9a02edfc` | 2026-11-18 | lx: synchronize_rcu() that gives up on readers still looking says so |
| `c57501fa` | 2026-11-18 | camera: every frame checked against the shape its buffer was sized for, sized anew when it changes |
| `dc1b071c` | 2026-11-18 | inject_sb: the layer dump held to its line, a gradient of many colours included |
| `e57d5e0d` | 2026-11-18 | lx: dma_alloc_coherent() of a page or more starts on a page, as Linux's does |
| `e6b95082` | 2026-11-18 | lx: destroy_workqueue() waits for the queue's work, and a queue that is full says so |
| `e6d02e58` | 2026-11-18 | var_store: files and directories opened without following a link, and the file saved the one that was looked at |
| `eb393582` | 2026-11-18 | tweaks: filters as Substrate's: Bundles against every loaded bundle, every kind given by default, Mode Any for one |
| `6f56ed92` | 2026-11-19 | inject_sb: the layer tree com.hakios.layers prints stops at a depth of 128 and says so (a deeper one, which any program could ask for, overf... |
| `814ee95d` | 2026-11-19 | var_store: a save writes the slot not holding the newest GOOD archive: the newest slot is read through before the other is written (once, as... |
| `9cebd473` | 2026-11-19 | lx_80211: the regulatory domain's two letters are printed only as printable ones (they may come from a beacon's country element through the ... |
| `e1afe9aa` | 2026-11-19 | loader: a write to darwin is good only if the device's cache flush after it is (the flush's answer was thrown away, and a read back from the... |

### Waiting: did not apply to Scion's copy (`git am -3` conflicts), or need a decision

| Upstream | Date | Subject |
|---|---|---|
| `40048f59` | 2026-11-16 | kernel/deps/host/bin/iig: find to xargs by NUL |
| `0a22ae6e` | 2026-11-17 | lx_80211: a frame being acted on finished before the station and the interface are freed |
| `1a3a82cf` | 2026-11-17 | cputop: the line of the busiest processes said empty when none took time, not the stack's bytes |
| `206b0c5d` | 2026-11-17 | ufs: the partitions the block layer kept are the ones the service offers, a refusal names its number |
| `442e0573` | 2026-11-17 | lx: a frame Darwin sends counted against the station's leaving, as a received one is |
| `762a2497` | 2026-11-17 | loader: the identity from the payload, when it carries one |
| `edb67e2a` | 2026-11-17 | loader: the device tree held to its room, and the identity to half of it |
| `0e04210d` | 2026-11-18 | lx: a group key renewed under the same number replaces the old one instead of being disabled with it |
| `11f0ccba` | 2026-11-18 | init: the preferences a boot-arg writes are made new, never through a link restored from the last boot, and handed to mobile by their descri... |
| `315493a1` | 2026-11-18 | lx: a stop during the Wi-Fi driver's start waits for it or leaves it to take itself down, and only one caller takes the driver down (it coul... |
| `3a937079` | 2026-11-18 | inject: waitstack=sb says a long turn after SpringBoard's main thread runs again, not while it is stopped |
| `5b813eab` | 2026-11-18 | metal: a surface whose Buffer is being destroyed has its kernel handle taken over by the new wrap, not wrapped by a second Bo and closed twi... |
| `5cfba6b0` | 2026-11-18 | the clock no longer starts in 1970 when nothing gives the time: the kernel starts it at its own build, and process 1 raises it to the newest... |
| `6f242b55` | 2026-11-18 | lx: a renewed group key copied as it came under the lock, not read by the work while the receive path writes it |
| `8d33595f` | 2026-11-18 | camera: a worker that ends by itself leaves the stream not running, and the next start makes another |
| `90ba7305` | 2026-11-18 | wifi: the WPA2 handshake keeps keys only from a message 3 that passed every check (a forged message 1 between the access point's no longer b... |
| `93266210` | 2026-11-18 | injected libraries: backboardd's QuartzCore addresses are read only once one is checked against the 16.5 build (on 26/27 they were read blin... |
| `cb0e6648` | 2026-11-18 | camera (cameracaptured): a stream's start and stop are one at a time and the frame thread says its own end (two could run, or one outlive it... |
| `d4657f0b` | 2026-11-18 | init: dark=1 and btnolpcore=1 written after the data volume's store is restored, not before it is laid over them |
| `d7170b7f` | 2026-11-18 | metal: a command queue lives while its command buffers do and the device waits for its queues (a queue freed by its last completion joined i... |
| `e76c820a` | 2026-11-18 | var_store: an attribute named as the store's own access control list not taken from the file |
| `f829a857` | 2026-11-18 | var_store: the keychain is kept out of the saved /private/var unless var_store_keychain=1 asks for it (its items reached the flash with a ke... |
| `027b5563` | 2026-11-19 | lx_80211: an authentication answer counts only while authentication is waited for and an association response only while it is (an authentic... |
| `062a3e30` | 2026-11-19 | inject: the main image's touch reader in backboardd (inject.c, libhakios.dylib) reads again after a signal, lets go of every finger and butt... |
| `45e0996a` | 2026-11-19 | var_store: a file's extended attributes are packed up to the count the load reads (4097 tiny ones on one file made every archive unloadable,... |
| `490ad345` | 2026-11-19 | lx: a delayed piece of work already pending, on its queue or waiting for its time, is left as it is and the answer is false, as Linux's (que... |
| `756a74f0` | 2026-11-19 | camera device: the frame worker frees the sensor's tuning when it ends (each start leaked it), the kernel's EIO is final for the run (every ... |
| `984e334e` | 2026-11-19 | metal: a draw into another slice or depth layer of a texture begins a pass of its own, a texture's level past its last is its last, the vary... |
| `99fad635` | 2026-11-19 | audio plug-in: a volume of NaN is silence (it passed every clamp and reached the kernel as the loudest), starting the haptic actuator no lon... |

### Left out: Apple's Secure Enclave, keys, attestation, FairPlay, App Store and Apple ID, IPSW decryption

None of this goes into a Scion repository. That includes upstream's `components/sep` (the Secure Enclave's keystore as the
kernel answers for it), `components/apple-id` (sign-in, the App Store, a Mescal signature "NOT rooted in the Secure
Enclave", MobileActivation answers and "hactivation"), `security/amfi`, `drivers/iokit/MiniAppleKeyStore.cpp`,
`darwin/tools/aea.py` (decrypting Apple's IPSW disk images) and the FairPlay jobs. Upstream's roadmap tracks this as
"Apple SEP work". The three commits that only add screenshots of Apple's user interface are left out as well.

| Upstream | Date | Author | Subject |
|---|---|---|---|
| `02b752be` | 2026-11-14 | vcvkk | App Store: the daemon's wait is not ours, and the bag arrives |
| `04517a97` | 2026-11-14 | vcvkk | App Store: appstored overflowed its stack in our own constructor, and the storefront has a second door |
| `0763b1d1` | 2026-11-14 | vcvkk | App Store: the restricted-region sync no longer blocks a thread for ever |
| `4b429174` | 2026-11-14 | vcvkk | App Store: every line of the store library names its process, and appstored is deadlocked |
| `56fc6ac7` | 2026-11-14 | vcvkk | App Store: the store's libraries on iOS 27, and the app no longer dies at launch |
| `5894da85` | 2026-11-14 | vcvkk | App Store: the 429 was a malformed request, not throttling, and the region answer is not what blanks the screen |
| `5fc7fd31` | 2026-11-14 | shimoku74 | docs: Darwin 27 in QEMU, and the IPSW decrypted with aea.py |
| `63ce50dd` | 2026-11-14 | vcvkk | App Store: a connection of its own times out, and the GUID decorations are not where the message comes from |
| `6c0d6151` | 2026-11-14 | vcvkk | App Store: the daemon's synchronous message is to accountsd |
| `6e6f400e` | 2026-11-14 | shimoku74 | darwin/tools/aea.py: decrypt a Darwin 26/27 IPSW's disk images on Linux |
| `7ade163f` | 2026-11-14 | vcvkk | App Store: what appstored waits for, and that this phone has no account at all |
| `7bc8ba39` | 2026-11-14 | vcvkk | App Store: the service naming is a diagnostic again, and appstored overflows its stack when it logs |
| `7eda746a` | 2026-11-14 | vcvkk | App Store: the catalog needs a token, and the token service answers 429 to this address |
| `857a01e9` | 2026-11-14 | vcvkk | App Store: the account wait cut on iOS 27, and the bag loads |
| `8e96fedf` | 2026-11-14 | vcvkk | App Store: nothing is blocked on XPC, and three readings of the log were wrong |
| `9523bf62` | 2026-11-14 | vcvkk | App Store: the token request is never made, and the headers dump says whose they are |
| `9dc16b30` | 2026-11-14 | vcvkk | App Store: accountsd watched without being changed, and it is idle |
| `a196116a` | 2026-11-14 | vcvkk | App Store: the stack overflow in appstored is not explained, and the last commit said it was |
| `d6572e58` | 2026-11-14 | vcvkk | App Store: where the storefront token is signed, and where it is not |
| `ec9a7162` | 2026-11-14 | vcvkk | App Store: the account hooks say which process answered, and what with |
| `00697701` | 2026-11-15 | vcvkk | App Store: the account the store asks a daemon for, and the two shapes the same selector has |
| `0711d84e` | 2026-11-15 | vcvkk | App Store: with the original asked first, AMS answers the account -- in the application too |
| `140646c7` | 2026-11-15 | vcvkk | App Store: the migration is not what makes the account types either |
| `15502756` | 2026-11-15 | vcvkk | store: promise tracker works -- names the stuck bootstrap promises; + resolve tracking |
| `1987c86a` | 2026-11-15 | vcvkk | App Store: FairPlay never answers, and this phone has no accounts database to speak of |
| `29b4f71b` | 2026-11-15 | vcvkk | App Store: the refusal has changed from "not authorized" to "not found", and the daemon's account has a storefront alrea |
| `2c918a0b` | 2026-11-15 | vcvkk | App Store: what 16.5 knew about the local account, and the account switch made safe by default |
| `337350d6` | 2026-11-15 | vcvkk | store: value-path force-resolve advances the bootstrap (refines the conclusion) |
| `3828a4e2` | 2026-11-15 | vcvkk | store: the account refusal, and three doors that are not the one |
| `41e1c750` | 2026-11-15 | vcvkk | App Store: the store asks for one bag and it is the advertising one, and the Jet daemon is told nothing |
| `4b5d62ad` | 2026-11-15 | vcvkk | store: name the maker of the refusal inside accountsd, and hand it an account |
| `4c00035f` | 2026-11-15 | vcvkk | App Store: the types and the account are there on a clean boot, and the screen is still Loading |
| `503b97ff` | 2026-11-15 | vcvkk | App Store: the real account has no storefront of its own, and the setter for it is written but not yet reached |
| `6278104b` | 2026-11-15 | vcvkk | store: force-resolve experiment (HAKIOS_FORCE_RESOLVE) -- external promise reject crashes |
| `6a56a2d8` | 2026-11-15 | vcvkk | App Store: accountsd refuses the client the account type, and that is why there is never an account |
| `6f14c88a` | 2026-11-15 | vcvkk | App Store: a second database connection never comes back, and the daemon's own one is there for the taking |
| `799349f5` | 2026-11-15 | vcvkk | store: dead-end dynamic probes + hung-bootstrap thread-dump driver |
| `7ce4af63` | 2026-11-15 | vcvkk | store: forward the account filter to its backing store; the code-9 wall falls |
| `8165ccf0` | 2026-11-15 | vcvkk | store: a JetEngine.Promise tracker for the bootstrap (did not fire; the approach's limit) |
| `91cbfdf1` | 2026-11-15 | vcvkk | App Store: the type was never missing -- this library was asking for a name that does not exist |
| `952433e9` | 2026-11-15 | vcvkk | store: answer cookie properties empty, without the daemon; the bag-fetch deadlock clears |
| `9b3a487b` | 2026-11-15 | vcvkk | store: fake the account-flags and region sync (negative result, kept for the record) |
| `9bba8dfa` | 2026-11-15 | vcvkk | App Store: accountsd's own initializer is reachable, and the next line after it never comes |
| `b0d9c8a5` | 2026-11-15 | vcvkk | store: track more promise constructors (confirmed: by-address Swift hooks crash the store) |
| `b7448411` | 2026-11-15 | vcvkk | App Store: our own answer is now what hands the application nothing -- the original is asked first |
| `c16e67bc` | 2026-11-15 | vcvkk | store: trace the bootstrap phases by address (HAKIOS_TRACE_BOOTSTRAP) |
| `c22028fe` | 2026-11-15 | vcvkk | App Store: the daemon's connection is not reached through either door that looked obvious |
| `ce23a8c9` | 2026-11-15 | vcvkk | store: the sync broker call the catalog waited on, and five hooks for it |
| `ce358c4f` | 2026-11-15 | vcvkk | store: ask the process what bundle it is, and what Security says it is signed as |
| `dac7c790` | 2026-11-15 | vcvkk | App Store: accountsd builds its own account types, and the account wall is down |
| `ea1bf4df` | 2026-11-15 | vcvkk | store: a knob to replace the bootstrap prerequisites with the empty array |
| `ecd0124b` | 2026-11-15 | vcvkk | App Store: nothing calls the method that would make the account types, and what that means |
| `f1d75875` | 2026-11-15 | vcvkk | store: name the maker of every error, and ask the keybag what jetpackassetd waits on |
| `f57133f5` | 2026-11-15 | vcvkk | store: correct single-hook JetEngine.Promise tracker + gate the crashing thunk hooks |
| `f6837638` | 2026-11-15 | vcvkk | App Store: accountsd can make every account type and never runs the code that would |
| `0b30c793` | 2026-11-16 | shimoku74 | components/sep: the Secure Enclave's keystore, as the kernel answers for it |
| `1d9b5463` | 2026-11-16 | shimoku74 | arch/arm64: include/, configs/xnu, tools/; security/ for the AMFI stand-in |
| `1da6de98` | 2026-11-16 | vcvkk | store: deferred cont-hook install -- app launches but ElleKit still crashes hooking the Swift continuation runtime (defi |
| `7e43fbbf` | 2026-11-16 | shimoku74 | components/apple-id: sign-in and the App Store in one place |
| `9da47449` | 2026-11-16 | vcvkk | store: type-spy -- promise #0 is the bootstrap ROOT (Promise<BaseObjectGraph>); gate is a suspended async await |
| `e0aea8f9` | 2026-11-16 | vcvkk | mad: port 16.5 hactivation to iOS 27 (device now reports Activated); activation ruled out for catalog |
| `e7cbb970` | 2026-11-16 | vcvkk | store: continuation tracker (HAKIOS_CONT_SPY) -- hooking the Swift continuation runtime hangs the app |
| `a8cd36cf` | 2026-11-18 | shimoku74 | locationd: -[CLMescalSigner initInSilo:] gives up at once, as Apple's own failure path does, instead of waiting for a fa |
| `bcf92ecf` | 2026-11-18 | shimoku74 | darwin image tools: the Wi-Fi passphrase stays out of the small image with SHARE=1 and is 0600 in it otherwise, an image |
| `c929d836` | 2026-11-18 | shimoku74 | init: a dasd budget file kept from an earlier boot loses its lastModulation from before 2001 before launchd starts (from |

## hakios-uefi (Scion-Project/hakios-uefi#2, open, not merged)

| Upstream | Subject | Decision |
|---|---|---|
| `4bb2a9f` | hakios slot, and an image's own environment | waiting: GPL-2.0+ (clear), but it adds a command that writes the phone's GPT attributes at every U-Boot start, which upstream deletes again in `7ef84fd` |
| `3207566` | hakios sum and hakios bootloader | waiting (same file) |
| `65621b6` | hakios slot never changes which slot is active | waiting (same file) |
| `c5ff339` | Use profile-based U-Boot for HakiOS devices | waiting: licence unclear (scripts, env, docs); removes the Mu-Silicium path |
| `df22ad3` | Remove Redwood package environment and U-Boot logo | waiting: licence unclear |
| `7ef84fd` | Make HakiOS U-Boot integration board-generic | waiting: licence unclear |
| `93e98ee` | Add FAT SD-card boot path and setup guide | waiting: licence unclear |

## hakios-sm7325 (Scion-Project/hakios-sm7325#2, open, not merged)

| Upstream | Subject | Decision |
|---|---|---|
| `2bc8dbe` | Normalize POCO X5 Pro device package | waiting: licence unclear |
| `26a1b52` | Use SM7325 device package names | waiting: licence unclear |
| `56f68b2` | Remove model name from device package | waiting: licence unclear |
| `b273005` | Organize build and storage documentation | waiting: licence unclear |
| `963967b` | Publish SM7325 device firmware payloads | **left out**: Qualcomm, Xiaomi, Awinic and FocalTech firmware, plus a prebuilt UEFI image and device trees |
| `e82aa55` | Remove empty vendor boot placeholder | left out (only undoes part of `963967b`) |
| `817ec5a` | Rename documentation directory to DOCS | waiting: licence unclear |

Neither repository changes what Scion builds: `devices/manifest.conf` pins `hakios-uefi` at `1c65228` and `redwood` at
`a8e7a87`.

## Not checked

- Upstream commits before 14 November that touch shared user-space files were grouped by area, not read one by one. Most
  are renames to the new layout or work for the Qualcomm boards; a generic fix among them could have been missed.
- Kernel-side fixes in `kernel/overlay` (for example `4c697dcc` sleh's IA-key line, `0733e3be` IOSurface's row bound,
  `d69daa10` a64emu's atomic accesses) belong in the fork Scion-Project/xnu, which this sync did not touch.
- Nothing was run on a phone.
