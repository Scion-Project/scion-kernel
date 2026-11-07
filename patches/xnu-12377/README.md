# patches/xnu-12377: the ios-26 base

Apple's `xnu-12377.121.6` plus `series`, applied by `xnu/apply-series.sh` when the OS profile's `base` is `xnu-12377`
(`OS_PROFILE=ios-26`, or `make blazer_defconfig`). Patches here are Scion's own, under the APSL like `patches/xnu-8796/`. Nothing
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
| `0080-hakios-drivers.patch` | the place for Scion's drivers, as on xnu-8796: `config/MASTER.hakios` read by `doconf` and a dependency of each component's configuration, `HAKIOS_DRIVERS` in the machine's kernel configurations, the HakiOS blocks of `bsd/conf/files` and `iokit/conf/files`. `tools/hakios-config.py kernel` fills them with the drivers whose `conf` names `xnu-12377` in `bases`: none yet |

The `VMAPPLE` kernel this makes is linked, not booted: `docs/two-bases.md` has the build and what it does not do yet (the
paravirtualized parts of `VMAPPLE`, which only Apple's hypervisor answers).
