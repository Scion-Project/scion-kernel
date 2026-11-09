# patches/xnu-12377: the ios-26 base

Apple's `xnu-12377.121.6` plus `series`, applied by `xnu/apply-series.sh` when the OS profile's `base` is `xnu-12377`
(`OS_PROFILE=ios-26`, or `make blazer_defconfig`). Patches here are Scion's own, under the APSL like xnu-8796's (`xnu/hakios.patch`). Nothing
derived from `ios-tegu` (no licence) and no GPL code goes in: `docs/two-bases.md`.

What the series does today is make Apple's open source build into a linked kernel for the `VMAPPLE` machine (arm64e, iPhoneOS
SDK) without a KDK: Apple builds the arm64 machine layer and a few parts of it into a binary library that comes with its Kernel
Debug Kit, and includes headers it does not publish. Scion's drivers have their place in its configuration (0080), but none is marked for this base yet, and nothing built from it has booted.

| File | What |
|---|---|
| `imports` | Apple files the tag includes but does not ship, fetched from another open release: the TrustCache library's headers from `xnu-8796.101.5` |
| `0010-makedefs.patch` | the KDK's `lib<machine>.os.<config>.a` is linked only when it is there |
| `0020-EXTERNAL_HEADERS.patch` | stand-ins for headers the open source includes but lacks: CoreEntitlements V2 (opaque types the kernel passes through), `CodeSignature/Entitlements.h` (four entitlement names) |
| `0030-config.patch` | `config/MASTER.arm64.vmapple`: iPhoneOS's configuration with `nos_arm_asm` and `nos_arm_pmap`, so the machine layer is compiled from the tree |
| `0040-osfmk-conf.patch` | `files.arm64`: the read-only region's files that are not in the open source out, Scion's two in |
| `0050-osfmk-arm64.patch` | `ppl/sart.h`, `ppl/uat.h`, `tunables/tunables.s` (empty: no SART, no Apple GPU, no Apple CPU tunables); `amcc_rorgn_scion.c` (no read-only region is locked); `machine_routines_scion.c` (no monitor, not an engineering sample); the two `CleanPoC_DcacheRegion_Force_nopreempt` forms in `caches_asm.s` |
| `0060-osfmk-arm-pmap.patch` | `pmap.c` without a monitor: three calls into the monitor's code the open source lacks, and a declaration before use |
| `0070-pexpert-arm64.patch` | `NO_CPU_OVRD` for `VMAPPLE`: the register's name is in Apple's private register header |
| `0080-hakios-drivers.patch` | the place for Scion's drivers, as on xnu-8796: `config/MASTER.hakios` read by `doconf` and a dependency of each component's configuration, `HAKIOS_DRIVERS` in the machine's kernel configurations, the HakiOS blocks of `bsd/conf/files` and `iokit/conf/files`. `tools/hakios-config.py kernel` fills them with the drivers whose `conf` names `xnu-12377` in `bases`: today `hakios.version.c` (0110) |
| `0090-native-pac.patch` | the PAC keys in the architectural ARMv8.3 key registers, one set per boot, instead of `VMAPPLE`'s paravirtualised `hvc` calls that only Apple's hypervisor answers |
| `0091-ns16550.patch` | a 16550 console in `pe_serial.c` for crosvm (AVF), beside the PL011 |
| `0092-no-impdef-regs.patch` | no Apple implementation-defined register after `wfi` on a non-Apple CPU |
| `0093-chkstk-bti.patch` | `__chkstk_darwin` with a `bti c` landing pad (the kernel calls it with `blraa`; Xcode's copy has none) |
| `0094-chained-fixups.patch` | `arm_init` applies and signs the kernel's `LC_DYLD_CHAINED_FIXUPS` (format 7): Xcode 26's `ld` no longer emits `__thread_starts` |
| `0095-physmap-slack.patch` | the physical aperture's worst-case twig alignment, as with a monitor (`arm_vm_physmap_init` asserted without it) |
| `0096-at-par-sync.patch` | an `ISB` before `PAR_EL1` is read after `AT S1E1W` in `mmu_kvtop_wpreflight` (UniXNU's `native-at-par-sync`) |
| `0097-pmap-sync-64bit-pa.patch` | `ptoa(pp)` where `pp << PAGE_SHIFT` dropped the high bits of a physical address at or above 4 GiB, in both pmaps' page-data sync (UniXNU's `zzx-pmap-sync-64bit-pa`) |
| `0098-native-pac-user-jop.patch` | with `SCION_NATIVE_PAC=1`: an arm64 thread runs at EL0 with the PAC keys off, and `ml_default_jop_pid()` is 1 so arm64e processes get signed pointers (UniXNU's `native-pac-arm64-user-jop`) |
| `0280-osfmk-minicrypto.patch` | minicrypto, the crypto provider of the xnu-8796 kernel (its files are in `xnu/files` there), kept here since the generated xnu-8796 series went |
| `0100-minicrypto.patch` | minicrypto's digests, AES and HMAC-DRBG built on this base (not the key store's parts); registered at the end of `SYSCTL`, because this base runs `EARLY_BOOT` after `PE_init_iokit`, whose platform UUID already takes a SHA-1 |
| `0110-hakios-core.patch` | what `drivers/core/hakios.version.c` needs here: `sys/hakios.version.h`, the `kern.hakios` sysctl node (`bsd/dev/hakios.base.c`, until `hakios.platform.c` builds on this base) and the call in `bsd_init()`; the first file of `drivers/` with `bases xnu-12377` |
| `0120-darwin27-syscalls.patch` | Darwin 27's eight new system calls (`pipe2`, `dup3`, `fchflagsat`, `getumask`, `aio_readv`, `aio_writev`, `fsctlat`, `guarded_ftruncate_np`), numbered as in iOS 27.0.1's kernel, so iOS 27's `libsystem_kernel` finds them on this base; `aio_readv`/`aio_writev` answer ENOTSUP for now. Scion's own code (`docs/ios27-starlte.md`) |

0090 to 0095 and 0098 to 0100 change what a kernel does only when it is built with `make kernel SCION_NATIVE_PAC=1` (off by default; 0100's
files are built either way, and register nothing without it). They came from Scion-Project/scion-avf's `vmapple/patches`
0001-0007 (PR 23), where the same changes booted a macOS-SDK `VMAPPLE` kernel on QEMU; the PAC design follows UniXNU's
(BSD-2-Clause-Patent, credited in NOTICE), and no UniXNU code is here. Why the kernel's crypto is minicrypto and not Apple's corecrypto,
and where the `SCION_NATIVE_PAC` kernel stops on QEMU today: `docs/two-bases.md`.

The `VMAPPLE` kernel this makes is linked, not booted: `docs/two-bases.md` has the build and what it does not do yet (the
paravirtualized parts of `VMAPPLE`, which only Apple's hypervisor answers).
