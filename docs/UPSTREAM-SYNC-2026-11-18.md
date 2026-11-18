# Upstream sync, 18 November 2026

What the projects Scion takes from (NOTICE, THIRD_PARTY_LICENSES.md, docs/two-bases.md, docs/star2lte-absorb.md) have done since
we last took from them, read on 18 November 2026 (UTC) to avoid doing their work again. One row per repository: what we last
took, its head now, what came since, what of it is relevant to Scion (the Galaxy S9, STARLTE, Exynos 9810 with Mongoose M3 and
Cortex-A55, ARMv8.0-8.2, no pointer authentication; the Pixel 10, FRANKEL, in QEMU; iOS 16.5 and iOS 27 user space), and the
decision. Read as data: nothing of these repositories was run. Out of this page's scope (other work owns them): the Metal stack
(UniXNU/AdrenoMetal, steelbrain/metal2vulkan, `tools/ios27/metal2/`, `tools/m2v/`, `tools/metal/`, `tools/mesa/`,
`userland/src/metalvk/`) and the camera.

Decisions: **take** (a pull request below), **already have**, **not applicable** (code for hardware or a design Scion does not
use), **not takeable** (licence), **left** (relevant, not taken in this pass; why).

## HakiOS (oosexgang), our parent

| Repository | Last taken | Head now | Since | Relevant | Decision |
|---|---|---|---|---|---|
| hakios-xnu-kernel `main` | merge base `63fc2b4633` (6 Nov) | `63fc2b4633` | 0 (GitHub compare: Scion's `main` is 0 behind) | nothing new | already have |
| hakios-xnu-kernel `mtk` | never | `a962798418` (10 Nov) | 7 commits, an unrelated root: HakiOS on the Infinix Hot 20 (MediaTek Helio G85): camera and Bluetooth skeletons for MediaTek, the MT6358 gauge, the MT6631 over STP/BTIF, porting docs | MediaTek drivers and docs only; no change to the common kernel or user space | not applicable |
| hakios-xnu-kernel `legacy-hackios` | (history before the fork) | `88513176b1` (26 Oct) | old Qualcomm modem/IPA work of the POCO X5 Pro, in Scion's history already | none | already have |
| hakios-mt6768 | never | `ad65e1f4b3` (10 Nov) | MediaTek device repository (Wi-Fi gen4m, camera S5K JN1, Bluetooth MT6631) | MediaTek only | not applicable |
| hackios-web | never | `72a5a9a93a` (5 Nov) | the HakiOS site | none | not applicable |
| hakios-sm7325 | never | `04fbfff933` (27 Oct) | POCO X5 Pro device layer | nothing since 27 Oct | not applicable |
| hackios-xnu-userland | never | `2573c4a8d8` (25 Oct) | "moved into the main HakiOS repository" | none | not applicable |
| hakios-uefi | never | `687fa6c6a5` (27 Oct) | POCO X5 Pro UEFI; nothing since 27 Oct | none | not applicable |
| hakios-metal | in-tree copy `hakios-metal/` | `d36f329e26` (5 Nov) | "moved: the driver is developed in the kernel's repository now" | Metal (out of scope) | already have (and out of scope) |

## UniXNU (github.com/UniXNU)

UniXNU rewrote its history on 17 November (its old commits are kept under `archive/pre-message-format-20261117/*`): `a9ec98c`,
the commit we took 0096-0098 from, is only on those archive branches now; the comparison below is of its trees, `a9ec98c` against
`main` `563595a` (18 Nov).

| Repository | Last taken | Head now | Since | Relevant | Decision |
|---|---|---|---|---|---|
| UniXNU `patches/xnu-12377/` (the series for every profile) | `a9ec98c` (8 Nov): 0090 (design), 0096, 0097, 0098 | `563595a` | one commit, `edd7aff` (10 Nov): `native-4k-shared-region-slide.patch` changed, `native-modload-lock-order.patch` and `native-union-readdir-eof.patch` new, `native-union-statfs-fsid.patch` macOS-only `#if`; the others' changes are the boot-argument prefix (`sheng-` dropped) | (1) the 4 KiB pager: their source page is released before a part is slid (the deadlock we fixed the same way in `sweet_sr_slide_subpage`, vm_shared_region_pager.c) and, new, the neighbour pages read for a 16 KiB slide page are put on the speculative queue; ours (`sweet_sr_read_src`, and the same reader `sweet_dyld_read_src` in vm_dyld_pager.c) leaves them on no queue, so they are never reclaimed. (2) modload lock order: a deadlock between a runtime kext load and late class registration; Scion's iOS 27 kernel loads no kexts (every driver is built in), so it cannot happen. (3) union readdir end: libc's readdir stops at the upper directory of a union mount; no union mount on the iOS 27 path (the 16.5 `init.c` mounts `/jb-bin` over `/flash/bin` as a union, where listing `/bin` would show only the upper half: noted, not taken) | (1) **take**, xnu PR (below), boot-arg `sweet_sr_spec=1`, off by default. (2) not applicable. (3) left |
| UniXNU macOS profile `profiles/os/macos-27.3-26C5035r/patches/xnu-12377/` (55 patches, all new since `a9ec98c`) | none | `563595a` | macOS 27.3 on their board: union root volume, Sandbox's Darwin 26 MAC hooks (`macos-end-rename-swap`, `macos-service-port-derive-check`), 16 KiB-signature hashing on a 4 KiB kernel (`cshash-native4k-*`, `memwired-cs-lazy-*`), the scaled 24 MHz timebase (`timebase-user-scaled-read`, `kperf-timebase-scale-no-division`: 19.2 MHz x 5/4), swap-in priority, kernel collection memory | the MAC hooks matter only with Apple's Sandbox kext (Scion has its own stand-in); the hashing work is for signatures Scion's root pipeline rewrites anyway (`fix-cs.py`); the timebase scale is 5/4 for a 19.2 MHz counter (the S9's is 26 MHz, a different problem); one generic diagnostic: `platform-error-name-address.patch` (`eb0a987`, 11 Nov) prints ESR and FAR in the panic for an implementation-specific (external abort) error | `platform-error-name-address`: **take**, xnu PR (below); the rest not applicable |
| UniXNU, Darwin assembler fact | none | ArmCpu `4168c17` (15 Nov) | "preserve ARM counter and GIC instructions on Darwin": in Darwin's arm64 assembler `;` starts a comment (the statement separator is `%%`), so `asm("isb; mrs %0, cntvct_el0")` assembles to `isb` alone | the same mistake is in the fork: `bsd/dev/starlte_smp.c:81,83` (`starlte_cpu_clock_report`, whose result its own neighbouring comment calls meaningless); the loaders (`xnuloader/sboot`, `avf/`) are built for ELF targets, where `;` separates, and are not affected | **take** (a fact; the fix is ours), xnu PR (below) |
| UniXNU desktop icon stores (`e23dc5a`, `49310a8`, `471976d`, 18 Nov) | none | `563595a` | a macOS boot job that drops `com.apple.iconservices.store` when the GPU plug-in's build record changes, so icons an earlier plug-in drew wrongly (System Settings' sidebar symbols as empty plates) are drawn again | our symptom (blank Settings symbols on the first boot after a new root, drawn on the second) is not a stale store: a new root brings a new `/private/var` (`tools/ios27/fullroot/steps.d/30-overlay.sh` writes into the image's `/private/var`), so there is no store from an earlier plug-in to drop; ours clears itself on the second boot, theirs persists until the store is dropped | not the same problem; not taken. A test that would tell: after a first boot, before the second, whether `/private/var/folders/*/*/C/com.apple.iconservices*` holds the symbols (drawn late, kept) or not |
| UniXNU other `main` work since 3 Nov (335 commits from 9 Nov) | none | `563595a` | Metal, Turnip, shader cache, macOS desktop, Sheng (SM8550) drivers, build and lab tooling, IPSW download | Metal: out of scope; the rest is their build system and their hardware | not applicable |
| SoftwareAES | `4a45326da5c2` (port in `scion/xnu-8796:iokit/Kernel/MiniIOAESAccelerator.cpp`) | `da07dd5` (11 Nov) | `6354bd7`: the Darwin 26 IOCryptoAccelerator ABI (`getAlgorithmName` in the parent) and a stand-in for key selector 0x8a4 (AppleImage4's boot-nonce entangling, derived from the unique chip ID); `da07dd5`: byte and time counters | Scion's iOS 27 kernel has no IOAESAccelerator and loads no AppleImage4 (its crypto is minicrypto, `osfmk/minicrypto`); the 16.5 kernel is Darwin 22, so the Darwin 26 ABI does not apply there; the counters are diagnostics. A device-key stand-in also sits in rule 4's territory (keys) | not applicable; counters left |
| ArmCpu | never (Scion has its own CPU, GIC and PSCI code) | `73c2360` (17 Nov) | platform sleep and GIC resume, owned-interrupt self-probes, idle and IPI statistics, PSCI version read as 32 bits, shared level SPIs | GICv3 kext code; the S9's GIC-400 is driven by Starbite's layer; the generic finding (the assembler's `;`) is the row above | not applicable (finding taken above) |
| ArmDTPlatform | never | `e8bd8d5` (10 Nov) | board clock registration, the calendar not before the kernel's build date, no wait for a clock the tree lacks | Scion's STARLTE already floors the calendar (`sweetepoch=`, fork `2c2043d`) | already have (the calendar floor); rest not applicable |
| XnuPrelink | never | `440589b` (12 Nov) | kernel-collection (fileset) building for macOS and 24A446/26C5035r imports | Scion builds one kernel Mach-O, no kernel collection | not applicable |
| DarwinBoot | never (`xnuloader/` does the job, docs/modular-kernel.md) | `5243c68` (11 Nov) | a memory window with excluded ranges for XNU, loader inputs recorded | UEFI loader of theirs; our loaders are xnuloader and sboot | not applicable |
| AppleKeyStoreState | never | `6ff4f11` (9 Nov) | key wrapping in software, boot-argument prefix | Scion has its own key store (SweetKeyStore, docs/ios27/KEYSTORE.md); key work is rule 4's | not applicable |
| RamBlock | idea only (`userland/src/var_store.c`, `tools/var-seed.py`) | `00a6e5d` (13 Nov) | persistent APFS root containers, concurrent direct mode, one-write bitmap reset, solid-state media | their kext's code paths; our store is a different implementation of the idea | not applicable |
| GopFramebuffer | never | `b0d812d` (11 Nov) | macOS display attributes, half-float frames, per-frame counters | UEFI GOP framebuffer kext; Scion has its own framebuffer | not applicable |
| UniXNU-sm8550-common | never | `2dc650c` (7 Nov) | SM8550 SMMU, video clocks, Bluetooth GENI UART | Qualcomm SM8550 | not applicable |
| UniXNU's other repositories (UniXNU-sheng, uefi-sheng, ShengNovatekTouch, ShengHidInput, ShengPreflight, Ath12kWifi, QcaBluetooth, QcomDwc3, QcomBaseband, QcomGeniSpi, Adreno740, AdrenoPM4, AdrenoFirmware, AirNir, VerifiedRoot, ArmSmmu4K, VirtioMmioBlock, VirtioMmioConsole, ArmVirtPlatform) | never | pushed 24 Oct to 18 Nov | the Sheng phone (SM8550: touch, HID, UFS, Wi-Fi 7, Bluetooth, USB, modem), Adreno GPUs, their VM devices and verified root | Qualcomm, Adreno or their virtual machine; Adreno/AIR work is the Metal stack's | not applicable |
| ShengUfs | read at `f2d6a4a` (facts, `drivers/storage/ufs/hakios.ufshc.h`) | `f020bd6` (15 Nov) | 25 commits: up to eight commands at once, Linux ufshcd's queueing, spinning through short waits, write caching, APFS container windows | our driver already keeps 31 slots of its own list (`hakios.ufshc.c` lines 13-34); the window and store work is theirs | already have (concurrency); rest not applicable |

UniXNU's licence: `BSD-2-Clause-Patent` for UniXNU itself (LICENSES/BSD-2-Clause-Patent-UniXNU.txt); its kext repositories say
`NOASSERTION` on GitHub (their LICENSE files were not needed: nothing of them is taken); ShengUfs BSD-2-Clause.

## Others

| Repository | Last taken | Head now | Since | Relevant | Decision |
|---|---|---|---|---|---|
| TheAndersMadsen/ios-on-pixel-10 `ios26` | `f04fd1f` (`devices/blazer/carried/`) | `803ec97` (14 Nov) | 27 commits: his lab tooling (`tools/hk_*.py`, `hkq.py`, `unixnu_record.py`, `drivers_check.py`; some MIT by header, some with no header), `devices/blazer` configs, ladders and evidence logs, notes (audio: the AoC parameter block; personas in QEMU unblocked by `RENAME_SWAP` on his varfs) | no kernel source is in its git (his kernel series lives outside it, docs/two-bases.md); the tools drive his phone lab and do not fit Scion's (`tools/hakios-device.py` does the device check here; `hk_device.py` says it took the idea from it) | not applicable (nothing new kernel-side to take for FRANKEL); facts noted |
| danstronics/ios-s9plus-exynos | `3a5b49c`/`2d09239` (docs/star2lte-absorb.md) | `2d09239` (6 Nov) | none | none | already have |
| dipa-studio/ios-sweet | the layer under patches 0003-0128 | (last push 23 Oct) | none | none | already have |
| dipa-studio/IOS-26-Hot | `tools/ios27/metal2/` (Metal, out of scope) | the repository is gone (404 on 18 Nov) | - | out of scope; our copy keeps its LICENSE | - |
| orhunnx/ios-a5xelte | the layer under 0003-0128, as ported by the Starbite author | `003f32f0f1` (3 Nov) | its own Mali GPU and runtime series (0242-0254: shader compiler deferral, AS0 job admission, upload arenas, an 800 MHz PLL try) | GPU queue work that StarbiteMali in the fork does its own way | left: needs reading against StarbiteMali by whoever owns the S9 GPU; MIT, takeable |
| wardevsa/ios-a5xelte `herolte`, `herolte-ios18` | 0200-0238 (permission, 8 Nov) | `2aafd7a0a1` (18 Nov) | iOS 18 on the Galaxy S7: hakios-gl hooks for CoreAnimation 22H352, root tuning, screen-off core parking | iOS 18 user space, not 16.5 or 27; core parking idea | left (no licence of its own, used with permission; nothing for iOS 27 found) |
| Project-Silicium/Mu-Silicium | `e51cd877e31c` (9 Oct; the S9's UEFI in scion-starlte) | `1077191b46` (18 Nov) | 42 commits: S5E9810's Hyp.bin updated twice (`3aa95a72`, `3b58e1d4`), the Mshc SD host for several hosts, KEP errata, new devices, Mu bumps | the S9's UEFI would change only by a rebuild and a phone test | left: no S9 fault is fixed by it; a UEFI rebuild needs a phone |
| Project-Silicium/Carbon-Hypervisor | the `Hyp.bin` inside our UEFI (GPL-2.0, binary with its source named) | `7a0ac02342` (8 Nov) | `929edb3` several Exynos targets; `bb4e57e` core numbers from a per-SoC MPIDR table (`platform/s5e9810.c`: 0x000-0x003, 0x100-0x103) | for the S9's MPIDRs the new table gives the same core numbers as the old rule (Aff1 0 or 1, Aff0 below 4) | not applicable (no change for the S9) |
| blacktop/darwin-xnu-build | (edits in the fork) | (last push 5 Sep) | none | none | already have |
| vcvkk/darwin-redwood | `eb9fdf5` (`userland/src/HakiosMetalCA.m`) | the repository is gone (404 on 18 Nov) | - | its licence stays "to verify" (THIRD_PARTY_LICENSES.md) | - |

## Pull requests

| Take | Repository | Pull request | Builds | Run on a phone |
|---|---|---|---|---|
| neighbour pages of a 16 KiB slide page on the speculative queue (`sweet_sr_spec=1`, off) | xnu | [Scion-Project/xnu#50](https://github.com/Scion-Project/xnu/pull/50) | STARLTE and FRANKEL: KERNEL OK | no |
| the Darwin assembler's `;` in `starlte_cpu_clock_report` | xnu | [Scion-Project/xnu#51](https://github.com/Scion-Project/xnu/pull/51) | STARLTE and FRANKEL: KERNEL OK | no (the function has no caller) |
| ESR and FAR in the implementation-specific error panic | xnu | [Scion-Project/xnu#52](https://github.com/Scion-Project/xnu/pull/52) | STARLTE and FRANKEL: KERNEL OK | no |

## How this was read

`git log`/`git diff` of clones made on 18 November (UniXNU and its kext repositories, ios-on-pixel-10, ios-s9plus-exynos) and the
GitHub API for the rest (branches, `compare`, commits since a date). Scion's side: scion-kernel `main` `553a198b` and the fork's
`scion/xnu-12377` `27a3e12`.
