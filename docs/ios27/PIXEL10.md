# iOS 27 on the Pixel 10 (frankel, Tensor G5 "lga")

Written 14 November 2026, offline: **no Pixel 10 was used for any of this, and nothing here has run on a phone.** Every result
below is from builds on the Mac mini and from QEMU, and says so. A QEMU result is not a phone result.

The order this page answers: what exists for the Pixel 10 on each line; what the Tensor G5 needs that the Galaxy S9's Exynos 9810
did not, and the reverse; the drivers; the iOS 27 kernel for this phone; how far it runs in QEMU; the root; the first test.

## Where each line stands

| Line | Kernel | What exists | Run where |
|---|---|---|---|
| iOS 16.5 (`ios-16.5`, xnu-8796, arm64) | `make frankel_defconfig` / `frankel_ufs_defconfig`, HakiOS's own GICv3 code | the lga boot shim ([scion-soc-lga](https://github.com/Scion-Project/scion-soc-lga)), `xnuloader` `platform_frankel`, read-only UFS for Google's host (`hakios.ufshc.c`'s `lga` entry, and `ufs_native_cold=1`), the frame kicker `lga_kick=`, boot images on the mini (`~/s9/p10/`, [scion-frankel](https://github.com/Scion-Project/scion-frankel) `FIRST-TEST.md`) | QEMU only: to SpringBoard off QEMU's UFS (6 November). Never on a phone |
| iOS 26 (`ios-26`, xnu-12377, VMAPPLE) | Scion's own: `make blazer_defconfig`, `SCION_NATIVE_PAC=1` | `devices/blazer` (Pixel 10 Pro) with the author's MIT `carried/` files, not built; [two-bases.md](../two-bases.md) | QEMU: to `image4 interface not available` (trust caches). Never on a phone |
| iOS 26, ios-on-pixel-10 (Anders Madsen, private `Scion-Project/scion-blazer`, used with permission) | its own xnu-12377 tree, carrying ios-tegu's changes (no licence: not importable) | iOS 26 to Home and Settings on **his Pixel 10 Pro**, five A725 cores, touch, display, UFS reads, a boot chooser (his `NOTES.md`, 4 November) | **On a phone** (his). The only Tensor G5 phone result there is, and not Scion's code |
| iOS 27 (`ios-27`, xnu-12377, the S9's layer) | **`MACHINE_CONFIG=FRANKEL`**, this page: Scion-Project/xnu `p10/frankel-ios27` | the board, its console and PSCI, the UFS driver, the loader's device tree for it, the root recipe changes | QEMU: Apple's iOS 27 `launchd` runs its boot tasks and stops at `mount-phase-1` (below). Never on a phone |

What blazer proves for this SoC, as facts (credit: Anders Madsen, ios-on-pixel-10): XNU 12377 and arm64e iOS user space run on
Tensor G5 cores; ABL's framebuffer and the panel can be driven; Google's UFS host can be read. What it did not do: run on the two
A520s (an erratum workaround bit it found clear) or the X4 (errata not handled), so it used the five A725s.

## Tensor G5 against Exynos 9810

### Pointer authentication and the instruction rewrite: not needed on this phone

The S9's root carries a rewritten shared cache and rewritten Mach-Os ([AOT-REWRITE.md](AOT-REWRITE.md), [FULLROOT.md](FULLROOT.md):
`a64e-rewrite`, `lse-rewrite`, the full AOT rewrite, `10-macho-a64e`) because the 9810's Mongoose M3 cores are ARMv8.0 and its
A55s ARMv8.2, without pointer authentication, while iOS 27's code is ARMv8.5+ arm64e. The Tensor G5's three core types, from Arm's
Technical Reference Manuals (ID register reset values; read 14 November from documentation-service.arm.com):

| Register field | Cortex-A520 (cores 0-1; TRM 102517 r0p3) | Cortex-A725 (2-6; TRM 107652 r0p2) | Cortex-X4 (7; TRM 102484 r0p2) | What it means |
|---|---|---|---|---|
| `ID_AA64ISAR2_EL1.APA3` | `0b0101` | `0b0101` | `0b0101` | address authentication, QARMA3, with EnhancedPAC2, FPAC and FPACCombined |
| `ID_AA64ISAR2_EL1.GPA3` | `0b0001` | `0b0001` | `0b0001` | generic authentication (`PACGA`), QARMA3 |
| `ID_AA64ISAR1_EL1.APA / API / GPA / GPI` | 0 | 0 | 0 | no QARMA5, no IMPLEMENTATION DEFINED algorithm |
| `ID_AA64ISAR1_EL1.LRCPC` | `0b0010` | `0b0010` | `0b0010` | `LDAPR`, `LDAPUR`/`STLUR` (the S9 rewrites these) |
| `ID_AA64ISAR1_EL1.JSCVT`, `FCMA`, `FRINTTS`, `BF16`, `I8MM` | 1 | 1 | 1 | `FJCVTZS`, `FCADD`/`FCMLA`, `FRINT32*`, BFloat16, int8 matrix (the S9 rewrites or traps them) |
| `ID_AA64ISAR0_EL1.TS` (A725, X4 tables) | | `0b0010` | `0b0010` | `CFINV`, `AXFLAG`/`XAFLAG` |

The A520's TRM gives its reset values as `xxxx` (configuration-dependent) and lists one defined value per field, the ones above.
So every instruction class the S9's rewrite exists for is implemented in hardware on all eight cores: **the shared cache and the
Mach-Os run as Apple ships them; no rewrite, no trap emulation.** Level of evidence: Arm's documentation of the cores, not a register
read of Google's silicon. The lga shim prints the boot core's `ID_AA64ISAR1/2_EL1` on its `cpu:` line, which on the first phone run
settles it for that core (QEMU `-cpu max` printed `pauth addr 5 generic 1`, the same encoding).

How PAC is used, chosen to match the S9 exactly: the FRANKEL kernel is plain arm64 like the S9's and leaves `SCTLR_EL1.EnIA/EnIB/EnDA/EnDB`
at 0. With the address keys off, `PAC*`/`AUT*` return the pointer unchanged and `BRAA`/`RETAA`/`LDRAA` act as their plain forms (Arm
ARM pseudocode: the enable check comes before any trap), so arm64e code behaves as it does on the S9's emulation, natively. Two
consequences handled:

- **`PACGA` has no enable bit.** It traps to EL2 when `HCR_EL2.API` is 0 and otherwise uses `APGAKey_EL1`, whose reset value is
  UNKNOWN per core. The shim already sets `HCR_EL2.API/APK` from `APA3` (scion-soc-lga `start.S`); branch `p10/ios27-pac-keys` now
  also zeroes all five EL1 key pairs on every core in `el2_setup`, so a `PACGA` value made on one core checks on another.
- **FPAC** faults a failed `AUT*` only when the key is enabled, so with the keys off it never fires.

Real PAC (per-boot keys, `SCTLR` enables) is possible later from the VMAPPLE layer's `SCION_NATIVE_PAC` code (`0090`, `0098`), behind
a boot argument; not done here.

### The rest

| | Galaxy S9 (Exynos 9810) | Pixel 10 (Tensor G5) | Consequence |
|---|---|---|---|
| Interrupts | GIC-400 (GICv2) | GICv3 with GICv4 redistributors, 256 KB a core | the layer's GICv3 path (SWEET's), frame stride `0x40000` |
| Timer | virtual timer PPI 11 (INTID 27), CNTFRQ 0 at handoff (26 MHz set by the ADT) | PPI 11, CNTFRQ set by firmware (38.4 MHz) | same INTID; no override needed |
| CPUs | 4 A55 + 4 M3, two MPIDR schemes, PSCI by SMC | 2 A520 + 5 A725 + 1 X4, MPIDR Aff1 = core, PSCI by HVC to the shim | SWEET's topology (Aff1) as is; PSCI conduit per board |
| Page size | 4 KB (A5XELTE layout) | 16 KB (SWEET's, iOS's own) | the iOS-native page size |
| Console | framebuffer, USB CDC-ACM (dwc3) | DesignWare 16550 at `0x0DB62000` (reg-shift 2, 32-bit) on USB-C SBU after `fastboot oem uart enable`; framebuffer; ramoops | `snps,dw-apb-uart` support added |
| Boot chain | S-Boot -> Starbite shim (GPL, private) -> XNU | ABL -> lga shim (Scion) -> xnuloader (Scion) -> XNU | Scion's own code end to end; an ADT built by `xnuloader` (`adt_layer=s9`) |
| Display | command-mode panel through DECON, kicked by the S9 driver | command-mode DSI (flea), DC9x00, frame kick at `OUTPUT0_START` | the 8796 line's `lga_kick` is not yet on this base (gap) |
| RAM | 4 GB, banks | 12 GB, ABL's map | xnuloader's window (largest free block, up to 6 GB) |

What the S9 has that the Pixel does not need: the AOT/PAC rewrite and its repair steps, PAC trap emulation (`sweet_a64e`, compiled
in but nothing reaches it), the LSE stubs, the Exynos PMU reset path for `sweetreboot`.

## Drivers

| Block | iOS 27 FRANKEL kernel (this branch) | Source of facts | State |
|---|---|---|---|
| UART | `pe_serial.c` 16550 with `reg-shift`/`reg-io-width` (`snps,dw-apb-uart`), PL011 for QEMU; xnuloader gives `/defaults serial-device` | Google `lga-lsio.dtsi` (via `lga-check`) | QEMU PL011: works. DW UART: written, not run |
| Timers / GIC | the layer's GICv3 (`pe_fiq.c`, SWEET), `GICR_PE_SIZE` 256 KB, timer INTID 27 | `lga.dtsi`, `frankel.c` | QEMU `virt,gic-version=4`: 8 cores, SGIs verified |
| CPU bring-up / PSCI | SweetCPU, PSCI `CPU_ON` by `hvc #0` to the shim | `frankel.c` (`psci_hvc`) | QEMU: 8/8 cores came alive through the shim. A520/X4 errata: open (blazer kept to the A725s) |
| UFS | Scion's `hakios.ufs.c` + `hakios.ufshc.c` (read only; `lga` entry, `ufs_native_cold=1` path behind its existing argument), `rd=ufsroot0 ufs_root=NAME` | `lga-check.md`, Google `ufs-google.c` | QEMU (`lga-qemu`, QEMU's UFS): GPT read, iOS 27 root mounted read only. lga entry: not run; `lga-check` mismatches M1-M2 (LPB state) still open |
| Display | framebuffer from boot_args (xnuloader's GOP via the shim), `sweet-fbcon`, AppleCLCD (`sweet,display`) | `pixel10-bringup.md` | written; QEMU had no framebuffer in these runs. **Frame kicker not ported** to this base: on the phone a command-mode panel may show nothing new |
| Touch | none (FocalTech on the AOC's SPI, bus negotiated with the AOC) | `frankel` README | missing |
| USB | none (DWC3 at 0x0C400000) | | missing; the S9's dwc3 console is refused on this board |
| PMIC / charging | none, read or write; the S9 PMIC entry points are answered "no" (`frankel_stubs.c`) | | missing by design; any write needs the owner's go |
| GPU | none: software rendering (CoreAnimation's CPU path, CoreImage software context) | PowerVR DXT-48-1536 | missing; expect no GPU for a long time |
| Wi-Fi / BT / modem | none | | out of scope |
| blazer `carried/` (NFC, compass, charger decode, haptics, exit trace, USB readback) | not built on this base yet: each needs hooks of ios-on-pixel-10's kernel that are not importable | MIT, Anders Madsen | not carried tonight |

## The kernel

Scion-Project/xnu branch **`p10/frankel-ios27`**, on the S9's integration branch `s9/p1-everyday-ios27` (read only; a draft PR against
it). One tree for both phones: FRANKEL is a machine configuration of the same layer, not a fork of it.

- `makedefs/MakeInc.def`: `MACHINE_FLAGS_ARM64_FRANKEL = -DARM64_BOARD_CONFIG_SWEET -DARM64_BOARD_CONFIG_FRANKEL`: the layer's generic
  board (SWEET: GICv3, PSCI, Aff1 cores, 16 KB pages) plus the lga facts in `pexpert/pexpert/arm64/SWEET.h` under
  `ARM64_BOARD_CONFIG_FRANKEL`. `config/MASTER.arm64.frankel` is the S9's option set, so user space sees the same kernel configuration.
- `bsd/dev/frankel_stubs.c` (option `scion_frankel`): answers the S9-only entry points that shared files call; the PMIC calls refuse;
  and the kernel **refuses to boot** with any boot argument that starts an S9 driver (`usb_gadget`, `cp`, `wifi_pcie`, `cam_s9`, `shub`,
  `panel`, `sweetreboot`, ...), since those would program Exynos addresses that are other blocks' registers here.
- UFS: `profiles/ios-27.copy` (scion-kernel `p10/ios27-frankel`) copies Scion's UFS driver and PCI bridge into the tree; only FRANKEL
  compiles them. `hakios.ufs.c` now maps an unmapped buffer (`buf_map`) instead of copying to NULL (the first QEMU root mount hit that).
- The SWEET board's Redmi-specific early framebuffer (`0x9c000000`) is off for FRANKEL.

Build (mini, as the S9's): the S9's `kbuild` with `MACHINE_CONFIG=FRANKEL`; `~/s9/agents/p10-ios27-port/kbuild.sh TAG FRANKEL`
(`out/TAG/kernel`, `mach.development.frankel`). It links; about 20.9 MB.

**The S9 kernel is unaffected.** Every edit to a file the S9 compiles keeps its line count (conditions extended on their own line,
new code in blocks the S9 does not compile, at the end of a file, or in new files), so no `__LINE__` moves. The S9 kernel was built from
the integration branch and from this branch at one build path and compared (results in the draft PR and in "Checks" below).

## The loader

scion-kernel branch `p10/ios27-frankel`, `xnuloader`: boot argument `adt_layer=s9` (off by default: the tree is unchanged for every other
kernel) makes the device tree this layer reads: 256 bytes of seed, `google,lga` after the identity's root compatible (SweetPlatformExpert),
`arm,armv9` on each CPU (SweetCPU), the GIC's `virtual-timer-intid`, the console UART with a phandle and `/defaults serial-device` (PL011 in
QEMU, the DesignWare UART on frankel), `disp0` (`sweet,display`) and `sweet-fbcon` from the firmware's framebuffer.

## QEMU: how far it runs

Mac mini, QEMU 11.1.3 (Homebrew), `-M virt,virtualization=on,gic-version=4 -cpu max`, TCG; the lga shim at EL2, xnuloader platform
`qemu-lga`, the FRANKEL kernel; `~/s9/agents/p10-ios27-port/qrun.sh`, `qufs.sh`. Logs: `q/r5-pid1.log`, `q/u3-launchd.log`, `q/u5-8cpu-launchd.log`.

| Run | What it reached |
|---|---|
| no root device | platform expert, CPUs, AMFI/Sandbox/Quarantine stand-ins, built-in HFS, networking; `Waiting on IOMedia Apple_HFS` |
| a 16 MB HFS RAM disk with Scion's own static init (no Apple code) as `/sbin/launchd` | process 1 runs, its checks (22, 8 fail on HakiOS-only sysctls), `etc/rc` |
| QEMU's UFS (the `lga-qemu` entry of the read-only driver) over a GPT whose `userdata` is an iOS 27 root image of the S9's, read only | GPT read, `BSD root: ufsroot0`, root mounted (unsealed); **Apple's iOS 27 `launchd` and `dyld` run** with the shared cache mapped; libignition; `hello, launchd`; boot tasks `exclaves-boot`, `fsck`, `mount-phase-1`; on 1 and on 8 cores |

**Where it stops, and why:** `mount-phase-1` runs `mount -uw /`; `mount_hfs` gets `Read-only file system` because the UFS block device
of this driver never writes (by design on this SoC), `mount` exits 1 and launchd, for which that task is critical, panics
(`mount[3] exited`). The S9 boots with its root read-write (`rootrw=1`, `ufsrw=1` and its own UFS writes); a writable root on the
Pixel is a decision for later (see the first-test plan: the system image goes in `userdata`, written by the owner, and the kernel
writes nothing). Two earlier stops were fixed on the way: the launchd hostname check (`kern.hostname` needs a platform binary: the S9's
`cs_enforcement_disable=1 cs_integrity_skip=1` arguments), and the NULL copy in `hakios.ufs.c`.

What QEMU cannot show: Google's firmware state (ABL, the SMMU, whether the UFS host is up), the real cores' behaviour (`-cpu max` is
not an A520/A725/X4: no errata), the panel, every timing.

## The iOS 27 root for the Pixel

The S9's recipe, `tools/ios27/fullroot/build-full-root.sh`, with `DEVICE=frankel` (scion-kernel `p10/ios27-frankel`; **not run**: the
mini has 40 GB free and the build wants 62; the QEMU runs used an existing S9 root read only, which is enough for the kernel):

- **No cache pipeline**: the stock `dyld_shared_cache_arm64e*` is staged as is (no a64e/LSE/PACGA/AOT rewrite, no `fix-cs`).
- **Skipped steps** (`lists/skip-frankel.list`): `10-macho-a64e` (PAC rewrite of Mach-Os), `85-textdata-restore` (repairs the rewrite's
  damage), `80-metal-ca` (the S9's Mali Metal bundle).
- **Kept**: `20-launchd` (the hardware daemon list needs a Pixel version: no Wi-Fi, BT, modem, camera, sensors), `40-prefs`,
  `50-xpc-disable`, `60-sileo`, `70-lsd-extkit` (no trust cache on either phone), `86-coreimage-sw-context` (no Metal/GLES: CoreImage
  needs its software context on the Pixel too).
- **Overlay**: `$IOS27_PRIVATE/overlay-frankel`: the S9's minus the Mali GL/Metal pieces (`libOSMesa`, `libsbgl`, `StarbiteMetal`) and
  the `cc_fips_test` stand-in (its reason, the PAC-rewritten corecrypto, does not apply). The brightness shim `b.dylib` is S9-only.
- `flash` refuses for `DEVICE=frankel`: nothing in the repository writes to a Pixel.
- **Display values**: the panel is 1080 x 2424. With the N104AP (iPhone 11) identity of the S9's iOS 27 root, the canvas is
  828 x 1792 points-pixels at @2x scaled by 1.304 to 1080 x 2337, centred (43 rows of black above and below), as the S9 scales the same canvas
  to its 1440 x 2960. MobileGestalt's screen values come from `/product` and `/chosen` (`display-scale` 2, `artwork-device-subtype`
  1792); `xnuloader`'s `identity.h` must be made from the iOS 27 N104AP device tree (`mkidentity.py`, the builder's own IPSW), not
  the 16.5 one the mini's copy has.
- **Graphics**: CPU only (CoreAnimation software path) until there is a PowerVR driver; expect the S9's pre-GPU speed or slower at
  first (no frame kicker yet on this base).

## First test (for the owner)

The decision to unlock is the owner's alone: **unlocking wipes the phone**, and it is his daily phone. Nothing here unlocks, flashes or
changes a Pixel, and no iOS 27 Pixel image has been made: the plan says what would be built and run, in order.

1. Before anything, the existing iOS 16.5-line probes (`~/s9/p10/FIRST-TEST.md`): `p10-probe-nokick.img`, `p10-probe.img`, then
   `xnu-frankel.img`, all `fastboot boot` (RAM only). They answer the questions iOS 27 needs answered first: does ABL take the image, does
   a frame reach the panel (kick or not), what does the shim's `cpu:` line say about PAC on the real cores, is the UFS host up.
2. Only then an iOS 27 kernel image, built the same way (`mkimage.sh` with this kernel, `adt_layer=s9`, the console on the DW UART or
   the screen), first with a RAM disk root (the run above with Scion's init), then `ufs_native=4 ufs_native_soc=lga ufs_native_ro=1
   rd=ufsroot0 ufs_root=userdata` once a Pixel root has been built and written to `userdata` by the owner (`fastboot flash userdata`,
   the one exception, as in the 16.5 plan). `audit-image.py` needs an ios-27 mode for these arguments before it can pass such an
   image (not done).
3. What he should see: the shim's bands and log, the loader's banner, the kernel's log on the screen (if frames reach the panel) and
   in ramoops; with the root, launchd's lines. Expected stop with a read-only root: `mount-phase-1`, as in QEMU.
4. Back to Android: every run is `fastboot boot`, so a restart (hold Power) brings Android back; `fastboot erase userdata` (or a factory
   reset) undoes the system image write. Re-locking wipes again.
5. Risks: unlock wipes (decisive for a daily phone: a second-hand Pixel 10 is the better test phone); a command-mode panel may stay dark
   (the frame kicker is not on this base yet: run the 16.5 probes first); the A520/X4 errata (start with `cpus=1`, then the A725s, as
   blazer did); everything else in the 16.5 plan (ABL refusing, SMMU, UFS host down). No PMIC, OTP, fuse, GSA, bl31 or RPMB write
   exists in this kernel or the shim (the shim's `audit.py` passes; the cold UFS path keeps its own audit).

## Checks

- Kernel builds: FRANKEL links on the mini (Xcode 16.4 toolchain `x164`, KDK 26.5 25F71), iOS target.
- **S9 byte check** (mini, 14 November, both builds with scion-kernel `p10/ios27-frankel` at one build path `dxb-s9cmp`, `NOWIFI`):
  the STARLTE kernel from the integration branch (`7fbd34c`) and from `p10/frankel-ios27` (`edec0d1`). Of 1226 objects, 1221 are
  byte-identical; `version.o` differs (build date, commit), and `KernelConfigTables.cpo`, `pe_serial.o`, `arm_vm_init.o`,
  `sweet_early.o` differ only inside their DWARF debug sections (source file checksums): their code and every other section are
  identical. The two kernels have all 93983 symbols at the same addresses and every section identical except `__TEXT,__const` (34
  bytes: the version string's date and commit, twice) and `__CTF`. `__CTF` is not reproducible on its own: two builds of the very same
  integration commit (`sbase`, `sbase2`) differ in 853540 of its bytes and in nothing else. Tool: `~/s9/agents/p10-ios27-port/kcmp.py`.
- scion-soc-lga shim: `make audit` passes with the key change.
- Not tested: anything on a phone; the DW UART path; the `lga` UFS entry on this base; the root recipe's `DEVICE=frankel` build.
