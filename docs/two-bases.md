# One tree, two XNU bases

Scion builds one kernel tree on two of Apple's XNU releases: **xnu-8796.121.2** (iOS 16.5, arm64) for phones whose CPUs lack
pointer authentication, the Galaxy S9 first, and **xnu-12377.121.6** (iOS 26, arm64e) for phones with FEAT_PAuth, the
Pixel 10 Pro first. Drivers, SoC layers and devices are shared; what differs per base is Apple's source, the patch series on it and,
where an interface differs, a shim. This page is the layout, what builds today, and what is left. `docs/modular-kernel.md` is the
background (UniXNU's layers and how this tree maps onto them).

## Layout

| Path | What it holds | Per base? |
|---|---|---|
| `profiles/<os>.conf` | the OS a build is for: XNU tag and commit, `base`, `machine`, `sdk`, `hfs`, `arch`, status | one per OS; several may share a base (ios-26 and ios-27 both name `xnu-12377`) |
| `xnu-src/<xnu_tag>/` | the kernel tree being built: Apple's tag, fetched pristine, plus the base's series. Not in git | yes, one per tag, side by side |
| `patches/<base>/series` | Scion's changes to Apple's files, in order (`patches/xnu-8796/`, `patches/xnu-12377/`) | yes |
| `xnu/hakios.patch`, `xnu/files/`, `xnu/patches/` | xnu-8796's work in its older form: `xnu/apply.sh` applies it, and gives the same tree as `patches/xnu-8796/series` | xnu-8796 only |
| `drivers/<class>/` | the drivers, by subsystem, with `conf` and `map` | shared; a driver says which bases it is built on (below) |
| `socs/<soc>/` | a SoC's numbers and layer (`exynos9810`, `lga` for the Tensor G5, ...) | shared |
| `devices/<name>/` | a phone: `device.conf` (now with `profile`), configs, image recipe | shared; the device names its profile |

`tools/os-profile.py` is the one place a path or tag per base comes from:

```
$ tools/os-profile.py list
ios-16.5   real             arm64  xnu-8796.121.2
ios-26     experimental     arm64e xnu-12377.121.6
ios-27     not-implemented  arm64e xnu-12377.121.6
$ tools/os-profile.py get src          # xnu-src/xnu-8796.121.2
$ OS_PROFILE=ios-26 tools/os-profile.py get series   # patches/xnu-12377
```

Which profile: `OS_PROFILE` if set, else the device's (`make NAME_defconfig` writes `.os-profile` from `devices/NAME/device.conf`'s
`profile` line, and removes it for a device without one), else `ios-16.5`. So `make starlte_defconfig && make kernel` is what it
always was, and `make blazer_defconfig && make kernel` is an xnu-12377 build.

`xnu/apply.sh`, `xnu/apply-series.sh`, `xnu/sync-drivers.sh`, `xnu/export.sh`, `tools/make-kernel.sh`, `xnu-env.sh`, the
`Makefile` and `tools/hakios-config.py` take the tree, series, machine configuration, SDK and HFS step from the profile. For
`ios-16.5` every one of them resolves to the value it had before, which is how the S9's kernel stays the same objects (below).
Tools that only make sense on xnu-8796 (`tools/kernel-delta.sh`, `tools/guard-test.sh`, `tools/soc-compile.sh`, the APFS tools,
`tools/new-workspace.sh`) still name its tree; `tools/kernel-version.py --darwin` still reads `VERSION`. Both are for later.

## Statuses

| Status | Meaning |
|---|---|
| `real` | builds, and a kernel from it has booted on a phone (ios-16.5: the Galaxy S9) |
| `experimental` | the base and its series apply; a build says on stderr that nothing from it has booted (ios-26) |
| `not-implemented` | named so the plan has a place; any build fails with the reason (ios-27) |

## Drivers across bases

A driver is source in `drivers/`, copied into the tree by `drivers/map`, and built when `config/MASTER.hakios` and the
HakiOS block of `bsd/conf/files` say so (`tools/hakios-config.py kernel`). Only xnu-8796's series carries the hooks those two
need, so on xnu-12377 `xnu/apply-series.sh` copies the drivers in and does not wire them: none is built there yet.

The plan, in order, each step on its own:

1. A `bases` line in `drivers/<class>/conf` (default `xnu-8796`), read by `tools/hakios-config.py`, so a driver is offered only on
   the bases it has been built on.
2. A series patch for xnu-12377 that adds `config/MASTER.hakios` to XNU's configuration and the HakiOS block to
   `bsd/conf/files`, as `patches/xnu-8796/0030-bsd-conf.patch` and `0130-config.patch` do on 8796.
3. Shims where an interface really differs, and only there (`docs/modular-kernel.md`: none has been shown yet). They go beside
   the interface headers the drivers already use (`sys/hakios.xnu.h` and the per-driver `*_xnu.c`), one directory per base:
   `drivers/core/xnu-12377/`. Expected first: `kalloc`'s typed forms, and pointer signing of the function pointers in
   `cdevsw` and IOKit tables on arm64e.
4. Drivers one at a time on xnu-12377, starting with the device-neutral ones (DWC3, the log, the UFS core), each marked with
   its base in `conf`.

## What builds

Built on the Mac mini on 7 November 2026 (Xcode 27.2):

- **ios-16.5, starlte:** built from this branch and from main with the same version suffix; every object compared (below).
- **ios-26, blazer:** see "The xnu-12377 build" below for how far it gets.

### The S9 stays the same

Both trees were applied from scratch and built with `HAKIOS_VERSION_SUFFIX` fixed, `make starlte_defconfig`, `make kernel`,
`-j4`. The SHA-256 of every `.o` under `BUILD/obj` is compared, and the kernels byte for byte. The result is in the pull request
that brought this page.

### The xnu-12377 build

Recorded in the pull request that brought this page: the command, the first error, and why it stops there.

## ios-on-pixel-10: what Scion may take

ios-on-pixel-10 by TheAndersMadsen (private here as `Scion-Project/scion-blazer`) boots iOS 26 on the Pixel 10 Pro. Its author
allows Scion to use **his own code** under Scion's licence (APSL for kernel files, MIT for tools), with credit. Third-party files
in it keep their own terms, and GPL never goes into a kernel file. Read from each file's header and its `docs/LICENSING.md` on
7 November 2026, at f04fd1f; this is a reading, not legal advice.

| Path in scion-blazer | Origin | Licence | Here |
|---|---|---|---|
| XNU 12377 kernel series (`combined/ios26-p11-all-in-one.patch`, against xnu-12377.121.6) | the author's work on top of `ios-tegu`'s port | **not in its git**: tegu's context lines carry no licence | no; rewrite on the pristine base (gaps) |
| `xnu/ios16-base/patches/` | xnu-8796 snapshot: Apple, HakiOS, tegu-derived, local, not split by origin | mixed, partly none | no |
| `dualboot/` (`readback.*`, `xnu_usb_readback.inc`, `android_handoff.*`, tests, `validate*.py`) | author's own | MIT by header | readback and its XNU adapter: `devices/blazer/carried/dualboot/` (not built) |
| `runtime/exit_trace/` | author's own | MIT (`LICENSE`) | `devices/blazer/carried/exit_trace/` (not built) |
| `runtime/ca_sampler/`, `runtime/bt_keychain_trace/` | author's own; user space | MIT (`LICENSE`) | no: user-space interposers, not kernel; candidates for `userland/` later |
| new `blazer_*` kernel files in tracked `research/**/*.patch` with an MIT header and nothing of tegu (`blazer_nfc*`, `blazer_cli10_bus.h`, `blazer_haptic_tap.h`, `blazer_haptic_service.h`, `blazer_mag_hw.*`, `blazer_charger.h`) | author's own | MIT by header | `devices/blazer/carried/` (not built) |
| other new kernel files in `research/` patches with no header (`blazer_usbhost*`, `blazer_memplan.h`, `hk_screen*`, `hk_bright_calib.h`, `blazer_aks_*.inc`, `sweet_touch_event.h`, `hk_physical_keys.inc`) | author's, but written inside tegu-derived classes (`Sweet*`) or with no header | unknown | no; ask, or rewrite |
| changes in `research/` patches to existing files (`SweetFramebuffer.cpp`, `SweetHID.cpp`, `blazer_gauge.c`, `blazer_bcm4390*.c`, ...) | the author's hunks on files that are not in git | the hunks are his; the files they apply to are not available | no |
| `shim/` 11 files `SPDX MIT` (`tgl_all.*`, `tgl_dcs.*`, `tgl_scanout_takeover.h`, `tgl_ufs_handoff.*`, `tgl_ufs_host.*`, `tgl_ufs_route.*`) | author's own; `tgl_ufs_host.c` follows the vendor's UFS order and values, `tgl_ufs_route.c` includes `tgl_ufs_link.h` | MIT by header | no (loader, not kernel); `tgl_ufs_route.c` cannot be taken without the GPL link file |
| `shim/tgl_ufs_link.c`, `.h` | Linux UFS code (Samsung, Linux Foundation, Google lines) | **GPL-2.0-only** | never in a kernel file; facts only |
| `shim/frame_tables.h`, `shim/dcs_tables.h`, `shim/dpu_quiesce.h` | generated from GrapheneOS vendor panel and DPU sources | **treat as GPL-derived** | no; facts only |
| `shim/start.S`, `loader.c`, `payload.h`, `laguna_usb.*`, `stamp.c`, `chooser*.c`, `tgl_usb_console.c`, `wdt.h`, `tgl_fastcore_s.S`, `shim.c` | local, tegu-derived in part (entry sequence, payload format, USB protocol) | **none** | no; design facts only |
| `shim/google-wdt-inspect.h`, `shim/probe.h`, `shim/fdt.*`, `shim/keys_laguna.*`, `shim/tgl_cpm.*` | register facts from Google's kernel source, local code | none stated | no |
| `shim/font.h` | generated from a `cp850-8x16` PSF font | **unknown** | no |
| `native/metal/isa/`, `native/metal/streams/` | author's own | MIT (`LICENSE` per directory); the AIR front end it links is external **LGPL-3.0-or-later** | no; GPU work, not this change |
| `tools/dualboot.py`, `tools/stage_dualboot_readback.py`, `tools/test_dualboot.py`, `tools/android_bootset.py`, `tools/test_android_bootset.py`, `tools/native_gpu_upstream.py` | author's own | MIT by header | no; candidates for `scion-installer` (GPT planner) |
| `tools/mkbootimg.py`, `third_party/aosp_bootimg/` | AOSP | **Apache-2.0** | no |
| `tools/image_tool.py` | local; `USER` boot arguments copied from tegu's build script | none | no |
| `tools/mkadt.py`, the other 80 files of `tools/`, `adt/` (YAML patches over Apple's device tree) | local and porting code, provenance partly recorded | none stated | no; design only |
| `*.md`, `docs/`, `.github/`, `research/` notes | local; some documents follow `ios-tegu`'s structure | none stated | facts only, in our own words |

### Gaps to write again

What the Pixel 10 Pro needs on xnu-12377 and cannot be taken, so is Scion's to write on the pristine base:

1. The platform: Tensor G5 early boot, GIC, PSCI, timers, the device-tree glue (that tree's series, tegu-derived).
2. The display path XNU uses (that tree's `Sweet*` framebuffer and surface classes, tegu-derived), and the boot shim's panel
   bring-up, whose tables are GPL-derived.
3. UFS: the link start-up (`tgl_ufs_link.*`, GPL-2.0-only) and the kernel's UFS owner the readback calls into.
4. Touch and buttons delivery (`SweetHID` hunks), the AKS user-session and key-wrap hooks (`SweetKeyStore` hunks).
5. The hooks the carried files need: `blazer_hw_port.h`, `blazer_cpmq_xnu.h`, the USB console core.

## Branding

User-visible strings in what this change adds say Scion; HakiOS's and ios-on-pixel-10's names stay where they credit the work.
The S9's own strings (`EXTRAVERSION = -hakios`, the banner) are not changed here, because the point of this change is that the S9
kernel stays the same objects; renaming them is its own change.
