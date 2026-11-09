# The kernel

Our changes to XNU `xnu-8796.121.2` (the kernel of Darwin userland 16.5, Darwin 22.5.0) are commits on Apple's tag in
[Scion-Project/xnu](https://github.com/Scion-Project/xnu), branch `scion/xnu-8796` (`docs/xnu-fork.md`): changes to Apple's
files, the new files at their paths in the XNU tree, HFS Plus, and the numbered patches (AVF platform, virtio-pci, display and
input, virtio-gpu, camera), one commit each. This directory holds what puts the tree together:

- `../drivers/`: the drivers, by subsystem (`drivers/README.md`); `apply.sh` puts them at their paths in the tree;
- `apply.sh`: fetches the fork at the commit the OS profile pins (`profiles/*.conf`, `fork_commit`) and lays the drivers and their
  configuration over it (`apply-series.sh` does the same for the xnu-12377 profiles; `fork.sh` is what both share);
- `stage/`: scripts that put what may not be in git into the tree at build time (the ios-27 profile's `fork_stage`);
- `sync-drivers.sh`: copies a driver edited in `drivers/` into the tree.

It builds on Linux with stock clang (no macOS, no Xcode) and on macOS with Xcode.

## Build

Linux (Arch): `sudo pacman -S --needed base-devel git curl clang lld python perl xz`, and
[theos](https://theos.dev/docs/installation-linux) in `~/theos` (or `$THEOS`) for its cctools
(`ld64`, `nm`, `otool`, `strip`) and `sdks/iPhoneOS16.5.sdk`. macOS: Xcode and MacPorts coreutils;
`xnu-deps/setup.sh` lays out `xnu-deps/theos-mac` itself.

```sh
xnu/apply.sh             # the xnu-8796.121.2 tag from apple-oss-distributions/xnu + our changes -> xnu-src/
xnu-deps/setup.sh        # dependencies, mig/unifdef/tcsh, the SDK overlay, two libraries
./xnu-build.sh -j8 -k    # about a minute on 12 threads
```

The kernel: `hakios-kernel` at the top of the repository (`make kernel` copies it there from XNU's `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`).
`./xl run` and `qemu/run-darwin.sh` take it from there (`prebuilt/` only when it is missing).

Things to know:

- `-k` is required: the build always ends with an error in `check_all_exports` (export lists), after
  the kernel has been linked. Look at the file's date, not at make's exit code.
- No spaces in the repository's path.
- After changing kernel headers that the two external libraries see: `xnu-deps/setup.sh --rebuild-libs`.
- Apple's DriverKit generator (`iig`) is closed. For xnu-8796, `xnu-deps/host/bin/iig` hands out the headers it would
  generate, ready-made in the macOS 13.3 SDK, and `xnu-deps/iig-impl/` holds stubs generated from the linker's undefined
  symbols (`xnu-deps/gen-iig-stubs.py`). For xnu-12377 on macOS it hands over to Apple's own `iig`, which Xcode has
  (headers and implementation both); on Linux it hands out the macOS 26.1 SDK's headers (docs/two-bases.md).
- A driver can be edited in `drivers/<subsystem>/`: `make kernel` runs `xnu/sync-drivers.sh`, which copies it into
  the tree when it is newer than the tree's copy (never the other way round). Everything else, and drivers edited
  in the tree, go back into `drivers/` by hand.
- The tree `xnu/apply.sh` makes is a clone of the fork at the profile's commit: change the kernel there (commit, push to the fork,
  then set the profile's `fork_commit` in a pull request here: `docs/xnu-fork.md`). Tools that only read the kernel's sources use
  `tools/xnu-tree.py` (a cached pristine checkout of that commit), not a built tree.

## What is added

| Files | What |
|---|---|
| `osfmk/arm64/gic_v3_irq.c`, `generic_arm64_support.c`, `psci_smp.c`, `loader_return.c` | the platform: GICv3, the timer, several CPUs through PSCI, the return to the loader with the log, the hypervisor watchdog |
| `osfmk/minicrypto/` | a replacement for corecrypto (MD5, SHA, HMAC, DRBG, AES) |
| `iokit/Kernel/GenericARMPE.cpp`, `MiniIOSurface.*`, `MiniIOMobileFramebuffer.cpp`, `MiniAppleKeyStore.cpp` | the platform expert and replacements for closed drivers: surfaces, the display, the key store |
| `bsd/miscfs/apfs/`, `bsd/miscfs/ramfs/` | read-only APFS and a writable RAM file system |
| `bsd/kern/kern_minimac.c`, `libkern/amfi/` | AMFI and Sandbox stubs |
| `drivers/input/hakios.input.c`, `hakios.goodix.c` | input: `/dev/hakios-touch`, the Goodix GT9916 touch controller |
| `drivers/power/hakios.cpufreq.c` | CPU clocks (Qualcomm EPSS) |
| `drivers/net/hakios.net.c`, `hakios.virtio_net.c` | the `en0` network interface |
| `drivers/gpu/adreno/hakios.adreno.c`, `drivers/gpu/hakios.gpu.cpp` | the Adreno 642L GPU |
| `drivers/wifi/hakios.wifi.c`, `hakios.wlfw.c` | Wi-Fi (WCN6750): shared memory, the radio's DSP, the driver |
| `drivers/modem/hakios.qmi.c`, `hakios.nas.c` | QMI and the NAS client |
| `drivers/modem/hakios.mpss.c` | the modem: firmware, file systems, IPA, SIM, registration |
| `drivers/audio/hakios.audio.c`, `hakios.adsp.c`, `hakios.speaker.c`, `hakios.mic.c` | sound through the audio DSP; the battery comes through it too |
| `drivers/haptic/hakios.haptic.c`, `hakios.camera.c`, `hakios.ufs.c` | the vibration motor, the cameras, the UFS storage |
| `bsd/pthread/pthread_kext_static.c` | the pthread kext linked into the kernel |

The changes also fix what keeps open XNU from building and running outside Apple: the makefiles
on Linux, the `BCM2837` configuration it is based on, bugs in abandoned code.

License: a derivative work of XNU, Apple Public Source License 2.0 (`LICENSE.APSL` at the top).
