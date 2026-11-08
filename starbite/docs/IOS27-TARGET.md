# The iOS-target STARLTE kernel on xnu-12377

Written 8 November 2026 from the builds of that day. `tools/build-xnu-12377.sh` builds the STARLTE tree the way
darwin-xnu-build does, as a **macOS-target** XNU (`SDKROOT=macosx`, `XNU_TARGET_OS_OSX`), and the board's
`config/MASTER.arm64.starlte` descends from `MASTER.arm64.MacOSX`. iOS 27 userland on it hit a run of Mac-isms, some of them
bugs, some only wrong answers. `tools/build-xnu-12377-ios.sh` builds the same tree as an iPhoneOS-target XNU instead
(`PLATFORM=iPhoneOS`, `XNU_TARGET_OS_IOS`, `XNU_PLATFORM_iPhoneOS`, `-miphoneos-version-min=18.5`, Mach-O platform IOS).

## Build

- `tools/build-xnu-12377-ios.sh` copies darwin-xnu-build's `build.sh` to `build-ios.sh` with `tools/dxb-ios.patch` (object
  directory `xnu-ios.obj`, `SDKROOT` from `XNU_SDKROOT` (default `iphoneos`), `UBSAN_CHECKS= UBSAN_RUNTIME=` (the iPhoneOS
  DEVELOPMENT configuration otherwise turns on minimal UBSan), the firehose library from `build/firehose-ios/`).
- The kernel file is `mach.development.starlte`, not `kernel.*`. Incremental builds take one to two minutes, a full build about
  25 at `-j4`.
- `build/firehose-ios/libfirehose_kernel.a` is the macOS-built `firehose_buffer.o` with its `LC_BUILD_VERSION` changed to iOS 18.5:
  `ld` refuses a macOS object in an iOS link. It was patched by hand; `vtool -set-build-version` fails on the object. Not scripted yet.
- `XNU_SDKROOT=macosx XNU_EXTRA_MAKEARGS=PLATFORM_FOR_CONFIG=iPhoneOS` selects a hybrid: macOS toolchain target, iOS configuration.

## The configuration (patch `0410`)

Added: `config_embedded`, `jetsam`, `freeze`, `phantom_cache`, `config_secluded_memory`, `vps_dynamic_prio`,
`config_skip_precise_user_kernel_time`, `config_taskwatch`, `config_io_compression_stats`, `routefs`, MULTIPATH.
Removed: `config_csr`, `config_csr_from_dt`, `config_arcade`, `config_ext_resolver`, `config_volfs`, `config_appledouble`,
`config_firmlinks`, `config_mount_vm`, `config_rosv_startup`, `config_basesystemroot`, `config_mount_prebootrecovery`,
`config_mnt_suid`, `quota`, `encrypted_swap`, HIBERNATION, NFS, `config_sched_auto_join`.
Kept: medium sizing (`xsmall` caps tasks at 512 and threads at 1536 below 3 GiB of RAM), `msgb_large`, System V IPC, audit,
`searchfs`, `nullfs`/`bindfs`/`mockfs`. Not added, because they would change behaviour: `config_enforce_signed_code`,
`config_library_validation`, `config_map_ranges`, `config_cs_validation_bitmap`, `xsmall`, `ubsan_minimal`.

## Source fixes the iOS target needed

- `0410`, `0411`: `sweet_hfs/core/kext-config.h`: the iOS branch turns on HFS content protection, which needs AppleKeyStore, so
  the OS X feature set is forced and `QUOTA` is 0; `hfs_encodings.h` was `!TARGET_OS_IPHONE` only.
- `0414`: `security/mac_base.c`: iOS makes `mac_policy_list` `SECURITY_READ_ONLY_LATE`, and Starbite's built-in AMFI, Sandbox and
  Quarantine stand-ins register after `machine_lockdown`: the first iOS-target boot panicked in `mac_policy_register`. Starbite
  boards keep the dynamic list. No other iOS-only read-only-late variable was found in the tree (`PE_putc` is set before lockdown).
- `0412`: `kern_exec.c`: the per-exec console line lived in the macOS-only branch that checks for an iOS binary.
- `0413`: `SweetFramebuffer.cpp`: `sweetfb_relbuf=1` publishes `SupportRelbufInfoCb` and answers IOMFB's release-buffer call, which the
  GPU compositing needs (from the GPU track's `relbuf.patch`).

## Mac-isms the iOS target removes

- `hw.machine`: the macOS target answered "iPad8,6" to every iOS-platform process (the iOS-app-on-Mac kludge); the iOS target
  gives the device-tree names.
- Every iOS process's `vm_map` was marked alien, and an iOS-binary spawn needed `unentitled_ios_sim_launch=1` (now a no-op).
- The per-process user address space: the macOS target gave every process `MACH_VM_MAX_ADDRESS` (63 GiB); the iOS target uses
  the device's size from `max_mem` (about 15 GiB at 1.5 GiB of RAM, more above 3 GiB), jumbo through entitlements. Escape
  hatch: boot-arg `arm64_maxoffset=0xFC0000000`.
- kauth: without `CONFIG_EXT_RESOLVER` the compat group GUIDs translate upstream and the credential's GUID is the compat one, so
  the GUID half of `0401` is redundant on this target; the systemusers (299) grant is still needed (credentials lack the group).
- `unlink()` giving EPERM to uid 501 is **not** a Mac-ism: every logged path was a directory (POSIX EPERM for `unlink(dir)`;
  Foundation then `rmdir`s or `removefile`s).

Still needed whatever the target: the 8 GiB shared region (`0403`), `security.mac.lockdown_mode_state_public` and the AMFI
stand-in sysctls and operations (`0402`), PRIV_DARKBOOT, Darwin 27's system calls (`patches/xnu-12377/0120`), the USB console and
the MUIC switch (`0400`).

## What was run

The iOS-target kernel (`k3`: patches `0410`-`0414` on the 8 November tree, with the GPU track's changes) booted on a Galaxy S9 far
enough to run launchd, backboardd and SpringBoard (the 8 November log shows them running, `STARLTE TOP` listing SpringBoard at 13 %
CPU); how smooth it is, and whether it differs from the macOS-target kernel in practice, was not measured. The tree that `0400`-`0424`
make together was not built as one before this repository: the macOS-target parity kernel and the iOS-target kernel were built
separately, from trees that differ by the patches of the other.
