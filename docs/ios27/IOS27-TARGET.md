# The iOS-target STARLTE kernel on xnu-12377

Written 8 November 2026 from the builds of that day; the build is `make kernel` since 9 November. The macOS-target build
(`tools/ios27/build-xnu-12377.sh`) builds the STARLTE tree the way darwin-xnu-build does, as a **macOS-target** XNU
(`SDKROOT=macosx`, `XNU_TARGET_OS_OSX`), and the board's `config/MASTER.arm64.starlte` descends from `MASTER.arm64.MacOSX`. iOS 27
userland on it hit a run of Mac-isms, some of them bugs, some only wrong answers. The iOS-target build makes the same tree as an
iPhoneOS-target XNU instead (`PLATFORM=iPhoneOS`, `XNU_TARGET_OS_IOS`, `XNU_PLATFORM_iPhoneOS`, `-miphoneos-version-min=18.5`, Mach-O
platform IOS), and it is the kernel of the `ios-27` profile.

## Build

```sh
export DEVELOPER_DIR=<Xcode 16.4 (or 26)>/Contents/Developer      # Apple's, yours
export KDKROOT=<the unpacked Kernel Debug Kit 26.5 (25F71)>        # Apple's, yours; not in this repository
OS_PROFILE=ios-27 JOBS=4 make kernel                               # -> ./hakios-kernel
```

It needs a Mac, Xcode, the Kernel Debug Kit, and `jq`, `gum`, `xcodes`, `cmake` and `ninja` on `PATH` (`TOOLS_BIN=<directory>` adds
one); without `DEVELOPER_DIR` or `KDKROOT` it stops with a message that says so. Nothing of Apple's is in the repository: the Kernel
Debug Kit stays where you put it. A full build takes about 25 minutes at `-j4`, an incremental one one to two. `make kernel` is
`tools/make-kernel.sh`, which for this profile (`builder dxb-ios` in `profiles/ios-27.conf`) runs `tools/ios27/build-xnu-ios.sh`, judges
the log the way it does for the other profiles (a failed step that is not a kext symbol-set check fails the build) and copies the
kernel to `hakios-kernel`. The pieces:

- `tools/ios27/dxb-common.sh` (shared with `build-xnu-12377.sh`) makes `build/dxb-12377` once: darwin-xnu-build at the commit
  `tools/ios27/dxb.pin` names (blacktop's, MIT; fetched by commit), its `build.sh` edited (plain arm64 firehose without ARMv8.1
  atomics, `-j4`, patches skipped) and the profile's tree put in it as `xnu/` by `xnu/apply-series.sh`: the fork's commit, what the
  profile's `fork_stage` scripts stage from the Kernel Debug Kit, the files of `fork_copy`. A changed profile (the fork's commit)
  asks for `FRESH=1 OS_PROFILE=ios-27 make kernel`; the shared drivers of `fork_copy` that you edit in `drivers/` are copied again by themselves.
- `tools/ios27/build-xnu-ios.sh` copies darwin-xnu-build's `build.sh` to `build-ios.sh` with `tools/ios27/dxb-ios.patch` (object
  directory `xnu-ios.obj`, `SDKROOT` from `XNU_SDKROOT` (default `iphoneos`), `UBSAN_CHECKS= UBSAN_RUNTIME=` (the iPhoneOS
  DEVELOPMENT configuration otherwise turns on minimal UBSan), the firehose library from `build/firehose-ios/`, `-j` from `XNU_JOBS`) and
  runs it.
- The kernel file is `mach.development.starlte` (in `build/dxb-12377/build/xnu-ios.obj/DEVELOPMENT_ARM64_STARLTE/`), not `kernel.*`.
- `build/firehose-ios/libfirehose_kernel.a` is the macOS-built `libfirehose_kernel.a` with the `LC_BUILD_VERSION` of its object changed to
  iOS 18.5 (`ld` refuses a macOS object in an iOS link; libdispatch's own iOS build does not find `Availability.h`, and `vtool
  -set-build-version` refuses the object, "not enough space to hold load commands"). `tools/ios27/firehose-ios.py` makes it, in place, three
  fields (platform, minos, sdk) and no other byte; the patch calls it (`XNU_FIREHOSE_IOS`) once dxb has built the macOS library, and
  `tools/ios27/firehose-ios-test.py` tests it. The object it makes is the one that was patched by hand before.
- `tools/ios27/build-xnu-12377-ios.sh` is the earlier name of this build: it runs `build-xnu-ios.sh` and copies the kernel to
  `out/kernel.development.starlte-12377-ios`.
- `XNU_SDKROOT=macosx XNU_EXTRA_MAKEARGS=PLATFORM_FOR_CONFIG=iPhoneOS` selects a hybrid: macOS toolchain target, iOS configuration.
- darwin-xnu-build is blacktop's (MIT, `github.com/blacktop/darwin-xnu-build`; credited in `NOTICE`); `dxb-ios.patch` is a patch to it.

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
- `0415`: `SweetSurface.cpp`: when `IOMemoryDescriptor::prepare()` refuses a task's image memory on the iOS target (CoreAnimation's `wrap_buffer`, Settings'
  80x80 icons), the surface takes a `copyin` snapshot into a kernel buffer instead of failing (the images do not change after they are wrapped). Phone-proven on `k4`.
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
stand-in sysctls and operations (`0402`), PRIV_DARKBOOT, Darwin 27's system calls (`commit 0120 of scion/xnu-12377`), the USB console and
the MUIC switch (`0400`).

## What was run

The iOS-target kernel (`k3`: patches `0410`-`0414` on the 8 November tree, with the GPU track's changes; `k4` adds `0415`) booted on a Galaxy S9 far
enough to run launchd, backboardd and SpringBoard (the 8 November log shows them running, `STARLTE TOP` listing SpringBoard at 13 %
CPU); how smooth it is, and whether it differs from the macOS-target kernel in practice, was not measured. The tree that `0400`-`0424`
make together was not built as one before this repository: the macOS-target parity kernel and the iOS-target kernel were built
separately, from trees that differ by the patches of the other.
