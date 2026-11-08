# Licences of `starbite/`

Read from each file's head and from where it came from, on 8 November 2026; this is a reading, not legal advice. Where a file's
origin or licence was not certain it is not here (`README.md`, "What is not here"). The repository's own rule applies on top:
XNU-derived files are APSL-2.0 (`LICENSE.APSL`), everything else of Scion's is MIT (`LICENSE`).

| Path | Licence | Origin and notes |
|---|---|---|
| `xnu/series-12377/*.patch`, as patches to Apple's XNU | **APSL-2.0** (`../LICENSE.APSL`) | A patch to Apple's XNU is a modification of it and cannot be MIT. Apple's own notices stay in the files the patches add or change (27 new files carry the APSL header: IOHIDFamily's event services in `0058` and its follow-ups). |
| `xnu/series-12377/0003`-`0081`, the staging scripts `0019`/`0020`/`0026-stage-*.sh` | APSL-2.0 for the XNU modifications; **MIT** for the donor's parts, notice in `LICENSES/MIT-ios-sweet.txt` | ios-sweet (dipa-studio, MIT), ported to xnu-12377 by the Starbite author. The staging scripts fetch Apple's libpthread and HFS (APSL-2.0) and BearSSL (MIT) at build time; nothing of theirs is stored here. |
| `xnu/series-12377/0082`-`0128` | APSL-2.0; MIT for the donor's parts, `LICENSES/MIT-ios-a5xelte.txt` and `MIT-ios-sweet.txt` | ios-a5xelte (orhunnx and A5XELTE contributors, MIT), which keeps ios-sweet's MIT notice. |
| `xnu/series-12377/0300`-`0398` | APSL-2.0 as patches; the **new files the patches create and that hold no Apple code** (`sweet_*.c`, `Sweet*.cpp`, `starlte_*.c`, `StarbiteMali.cpp`, `StarbiteAccelerator.cpp` and the like) are the Starbite author's own and may also be taken under **MIT** (`LICENSES/MIT-Starbite.txt`) | The Starbite author's port. He agreed on 8 November 2026 to publication under MIT of his code. |
| `xnu/series-12377/0400`-`0414` | APSL-2.0 | Scion's changes (patch authors Claudius, Scion). `0400` adds `hakios.*` driver files from this repository's `drivers/`, APSL-2.0, and `starlte_usbcon.c`, `starlte_muic.c` written on them. They name the Samsung kernel's register facts (clocks, LDO numbers, opcode sequences) as facts; no Samsung GPL code is copied into them. |
| `xnu/series-12377/0420`-`0424` | APSL-2.0 | The Scion drivers ported from `drivers/` (CPU governor, TMU, PCIe, DPU), with the changes each file's head states; default off. |
| `shim/starlte/*` | **GPL-2.0-only**, as each file's `SPDX-License-Identifier` line says | The loader's own parts. The Starbite author agreed to MIT for his code; the files stay GPL-2.0-only as marked until he says per file which are wholly his (several follow the Linux and Samsung drivers for the DWC3, UFS, MUIC and fuel gauge and could not be MIT whatever he says). The loader is a program of its own that the phone runs before the kernel; none of it is copied into a kernel file, and nothing GPL goes into an APSL file. |
| `tools/*.sh`, `*.py` (build, packing, root helpers), `tools/a64e/`, `tools/memtest/`, `tools/touchspy/`, `tools/gpu/*.c`, `tools/gpu/*.sh`, `docs/*.md` | **MIT**, the Starbite author's (`LICENSES/MIT-Starbite.txt`) | `tools/usb-fastboot/host_test.c` is **GPL-2.0-only** by its head (it tests `shim/starlte/fastboot.c`). `tools/gpu/panfrost_drm.h` is MIT (Broadcom, Collabora; the DRM uAPI, by its head). |
| `tools/build-xnu-12377-ios.sh`, `tools/adt-extra-ram.py`, `tools/ca-gpu/build-sbca27.sh` | MIT | Written for this work. |
| `tools/dxb-ios.patch` | **MIT** | A patch to blacktop's `darwin-xnu-build` `build.sh` (MIT, Copyright (c) 2023-2025 blacktop); the context lines are his. |
| `tools/ca-gpu/sbca.c`, `install-sbca*.sh`, `build-sbca.sh` | MIT | The Starbite author's, with the iOS 27.0.1 offsets and addresses of the iOS 27 port (numbers read from the builder's own restore image; no Apple code). |
| `tools/ca-gpu/hakios/` | **MIT**, `hakios/LICENSE.hakios` | HakiOS's EAGL bridge (the HakiOS authors), with iOS 27 table changes. Scion's copy is `userland/src/eagl.c`. |
| `tools/mesa-ios/*.patch` | **MIT** (Mesa's terms for the files they change) | Patches only; Mesa is fetched. `starbite-drm/` follows HakiOS's `hakios-drm` shim (MIT). |
| `LICENSES/` | | The notices the MIT parts need. |

## Not asserted

- The SPDX line of a `shim/starlte/` file is the author's; it is not a claim that nothing in it follows a GPL source.
- The names "Starbite", "Samsung", "Galaxy", "Apple", "iOS" and the like are their owners'.
