# Starbite: iOS 27 on the Galaxy S9 (starlte), on an iOS-target xnu-12377 kernel

This directory holds the source of the iOS 27 port to the Samsung Galaxy S9 (SM-G960F, Exynos 9810, `starlte`): iOS 27's user
space on Apple's open-source XNU `xnu-12377.121.6` (the newest public XNU; iOS 27.0.1's own kernel, `xnu-13432.2.10`, is not
published), built as an iPhoneOS-target kernel for the S9's CPUs. It started as **Project Starbite**, a multi-boot system for the
S9 by its author (the "Starbite author", credited below), and is published here with his agreement. The kernel series is on the
`ios-26` / `xnu-12377` base of `docs/two-bases.md`; the fixes after the import are patches on top of it, each in its own commit.

You bring your own IPSW. Nothing of Apple's is in this repository: no kernel, kernelcache, dyld cache, device tree, disk image,
key, symbol dump or disassembly, and no image built from them. Nothing of Samsung's either.

## Status (8 November 2026)

- iOS 27.0.1 (24A446) userland boots on a real Galaxy S9 to SpringBoard's home screen, with Settings usable. It is slow: the
  screen was composited by the CPU, there is 1.5 GiB of RAM visible (one DRAM bank) and the arm64e user space runs on a CPU
  without pointer authentication through emulation of the instructions that trap.
- GPU: CoreAnimation rendering through OpenGL ES on the Mali-G72 (Mesa Panfrost behind HakiOS's EAGL bridge; `tools/ca-gpu`,
  the iOS 27 port in `sbca.c`) is ported to iOS 27 and is being brought up. Frame pacing is in progress.
- The parity work with Scion's iOS 16.5 kernel (extra RAM, CPU governor, TMU, Wi-Fi stage 1, DPU scan-out) is in as patches
  `0420`-`0424`, **off by default** behind boot-args, and not yet booted on a phone.
- What was run, and where, is in each commit message. This repository's own checks (`docs/checks.md`) do not boot anything.
  No phone was used to write this page: the status above is the project's, as of that date.

## What is here

| Path | What |
|---|---|
| `xnu/series-12377/` | The patch series on `xnu-12377.121.6`: `0003`-`0128` (the ios-sweet and ios-a5xelte layers), `0300`-`0398` (the Starbite author's STARLTE port and its macOS 26 / iOS 26 bring-up), `0400`-`0424` (the iOS 27 fixes, below). `README.md` there says how each earlier patch was ported. |
| `shim/starlte/` | The Starbite loader's own parts: boot menu v7, the OS store and catalog, the recovery menu and POST, USB device mode with a fastboot subset, the S9's UFS and microSD readers, battery and MUIC. GPL-2.0-only, see `LICENSES.md`. The loader's entry, framebuffer and payload code is **not** here (below). |
| `tools/build-xnu-12377.sh` | Sets up `build/dxb-12377` (blacktop's darwin-xnu-build at a pinned commit), applies the series and builds the STARLTE kernel as a macOS-target XNU. |
| `tools/build-xnu-12377-ios.sh`, `tools/dxb-ios.patch` | The same tree built as an **iOS-target** XNU (`SDKROOT=iphoneos`). `docs/IOS27-TARGET.md` says what that changes. |
| `tools/make-xnu-boot.sh`, `mkimage-ios26.sh`, `mkpayload-ios26.sh`, `starlte-disable-jobs.py`, `starlte-job-log.py`, `adt-reseed.py`, `adt-extra-ram.py` | Packing the payload and boot image, and small root and ADT helpers. They call `mkpayload.py` and the ADT modules of the `ref/ios-herolte` tools, which are not here (below). |
| `tools/ca-gpu/` | `sbca`, the library the kernel inserts into `backboardd` so CoreAnimation renders through OpenGL ES; its iOS 27.0.1 offsets; HakiOS's EAGL bridge (`hakios/`, MIT) it loads. |
| `tools/mesa-ios/`, `tools/gpu/` | Mesa patches (Panfrost and PanVK for iOS), the DRM shim over the kernel's `StarbiteMali` user client, Mali-G72 tests. |
| `tools/a64e/` | The emulation of the arm64e and ARMv8.1+ instructions the S9's cores lack, and its tests. |
| `tools/memtest/`, `touchspy/`, `usb-fastboot/` | On-device test programs and the fastboot host test. |
| `docs/` | `IOS27-TARGET.md`, `PARITY.md` (this work), and the author's pages on the loader's recovery menu, its USB console and fastboot subset, and the microSD root. |
| `LICENSES.md`, `LICENSES/` | Licence per path, and the notices that go with the MIT parts. |

## What is not here, and why

- **The S7 layer, patches `0200`-`0238`, and the loader's core.** Starbite builds on the Galaxy S7 (herolte) port in
  `wardevsa/ios-a5xelte` (branch `herolte`), which states no licence. The 39 patches `0200`-`0238` and the loader files that
  say "Based on ios-a5xelte shim/herolte" (`xnu_boot.c`, `xnu_boot.S`, `fbtext.*`, `probe.*`, `build.sh`, `image.ld`), the
  `mkadt_starlte.py`, `starlte-pack-boot.py`, `starlte-flash-root.py`, `starlte-log-pull.py` and `stage-ios-root.sh` that follow its
  tools, and `shim/fdt.*` are therefore withheld. **The kernel does not build from this tree alone**: patches `0300` and up (and
  `0400` and up) apply on top of the S7 layer, 12377-rebased. When that author licenses his work, or Scion has its own
  equivalent, they go in. Until then you need your own copy of `ref/ios-herolte` and of its rebased patches.
- The other operating systems of the Starbite tree (Linux, ChromeOS, Android, Windows on ARM, macOS Ventura, iOS 14, iOS 18), its
  OS store and menu assets, splash and boot-picture tooling, fonts and logos (third parties' trademarks and licences), and the
  development log.
- Anything that writes to the phone: flashing, root-image writing and TWRP scripts. Use `scion-installer`.
- Scion's drivers are not duplicated except where a patch has to carry them (the `hakios.*` files patches `0400` and
  `0420`-`0424` add are the ones the iOS 27 tree booted with, copied from or adapted from this repository's `drivers/`; each
  says so in its head).

## Build

Needs a Mac. Apple's pieces are yours to bring.

1. Xcode 16.4 (or 26), `jq`, `gum`, `xcodes`, `cmake`, `ninja`, Python 3; the Kernel Debug Kit 26.5 (25F71) from Apple's
   developer downloads, unpacked (`pkgutil --expand-full`); the S7 layer above.
2. `DEVELOPER_DIR=... KDKROOT=... tools/build-xnu-12377.sh` (about 20 minutes the first time). It fetches `xnu-12377.121.6` and
   the pinned HFS, libpthread and BearSSL sources, applies `xnu/series-12377` in order and builds `STARLTE`.
   Then `tools/build-xnu-12377-ios.sh` for the iOS-target kernel (needs the `firehose-ios` library its head describes).
3. Pack the payload and boot image (`tools/make-xnu-boot.sh`), and the root from your own IPSW: Apple's system and cryptex disk
   images are AEA-encrypted, and decrypting them and building the HFS root (it needs root on the Mac) is your own step; see the
   repository's `sysimage/unpack.sh`. Boot arguments new with this work: `usb_gadget=5`, `sweetacl=1`, `sweeteacces=1`,
   `ufsrootoff`/`ufsrootlen`, `sweetfb_relbuf=1`, and (off by default) `extra_ram=1`, `cpufreq_gov=1`, `tmu=1|2`, `wifi_pcie=1..5`,
   `dpu_probe=1`, `startouch=<prio>`, `startouch_ms=<ms>`.
4. Flash the boot image with your own means (the S9's BOOT partition only; the project never writes EFS, RIL, RPMB, TIMA or
   PARAM) and the root to USERDATA, then watch the log on the USB-C console (`usb_gadget=5`).

The series 0400 and up were made from the working trees of three runs on 8 November 2026 and checked by exporting them from git; the
whole series was not rebuilt from this repository, for the reason in the previous section.

## Credits

- **The Starbite author**: Project Starbite, the STARLTE port of XNU and the loader this directory is based on. The credit
  is a placeholder ("Starbite author") until he gives the name or account he wants used.
- **Scion** (this repository) and **HakiOS** (`oosexgang/hackios-xnu-kernel`): the DWC3 gadget console, Exynos drivers, the EAGL
  bridge over Mesa, and the design of the parity work.
- **ios-sweet** (`dipa-studio`, MIT) and **ios-a5xelte** (`orhunnx` and A5XELTE contributors, MIT): the XNU port to a phone with
  no Apple hardware and its Exynos 7580 layer, patches `0003`-`0128`. The S7 layer (`wardevsa`) is the one withheld above.
- **Mesa** (Panfrost, PanVK; MIT), **Mu-Silicium** (the UEFI Scion's own S9 path boots through), **blacktop**
  (`darwin-xnu-build`, MIT).
- Apple's open-source XNU, HFS, libpthread and IOHIDFamily (APSL-2.0).
