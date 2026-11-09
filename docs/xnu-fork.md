# The kernel is a fork: Scion-Project/xnu

Scion's kernel source is [Scion-Project/xnu](https://github.com/Scion-Project/xnu), a fork of Apple's
`apple-oss-distributions/xnu`. Scion's changes are ordinary commits on Apple's release tags, one branch per base, instead of a
patch series that a build applies to a fresh copy of Apple's tree. This repository keeps what is not the kernel tree (devices,
SoC layers, the driver configuration, the loader, user space, tools, docs) and names, per OS profile, the commit of the fork a
build takes.

| Profile | Branch | On Apple's tag | What the branch's commits are |
|---|---|---|---|
| `ios-16.5` | `scion/xnu-8796` | `xnu-8796.121.2` | `xnu/hakios.patch`, `xnu/files/`, HFS Plus (Apple's `hfs-627.100.6` files, then `xnu/hfs/hfs.patch`), the numbered patches `xnu/patches/*.patch`: one commit each, in the order `xnu/apply.sh` applies them |
| `ios-26` | `scion/xnu-12377` | `xnu-12377.121.6` | the TrustCache headers of `xnu-8796.101.5` (`patches/xnu-12377/imports`), then `patches/xnu-12377/series`, one commit per patch |
| `ios-27` | `scion/xnu-12377-starlte` | `xnu-12377.121.6` | `patches/xnu-12377/series.ios27`, one commit per line: what darwin-xnu-build's edits and the staging scripts make (Apple's libpthread and HFS and BearSSL, by commit), `xnu/files-12377/`, the `copy` lines, every patch |

Each commit's message carries the patch's own description, the file of this repository it came from (and the commit of this
repository it was taken at), and for the ios-27 branch the layer it belongs to (ios-sweet, ios-a5xelte, wardevsa's Galaxy S7
layer, the Starbite author's STARLTE port, Scion's), as `LICENSES.md` and `NOTICE` record them. The licences are the files' own:
the fork is APSL-2.0 as Apple's XNU is; files of other authors keep theirs.

## How a build takes it

`profiles/<os>.conf` names the fork (`xnu_fork`), the branch (`fork_branch`) and the commit (`fork_commit`). `xnu/apply.sh` (and
`xnu/apply-series.sh`, which it calls for an xnu-12377 profile) then fetches that one commit (`git fetch --depth 1`) instead of
Apple's tag and the series, checks it out on the branch `hakios`, and adds what it always added after the series: the drivers of
`drivers/map` and of the SoC and device repositories checked out, and the drivers' configuration (`tools/hakios-config.py
kernel`). `make kernel` is unchanged for the user: `make starlte_defconfig && make kernel` builds the S9's iOS 16.5 kernel,
`OS_PROFILE=ios-26 make kernel` the VMAPPLE kernel, and `tools/ios27/build-xnu-12377.sh` the iOS 27 S9 kernel, each from the fork.

What may not be in git is still staged at build time, by the scripts the profile names in `fork_stage`: for ios-27 that is
CoreEntitlements' V2 headers, which come from the Kernel Debug Kit you bring (`patches/xnu-12377/starlte/blacktop/00_setup_coreentitlements.sh`,
`KDKROOT`). The libpthread and HFS staging copies the SDK's `TargetConditionals.h` (Apple's, APSL-2.0 by its header) into the
tree; on the branch it is the copy from Xcode 27.2's macOS SDK, the one the iOS 27 kernel was built with.

Scion's shared drivers stay in this repository, as for the other profiles (`drivers/map`): for ios-27 the files that
`series.ios27`'s `copy` lines named are copied into the tree at build time from `drivers/` and `socs/` (`profiles/ios-27.copy`, the
profile's `fork_copy`), and the branch does not keep a second copy; `hakios.s6e3ha8.c` is the exception until its ios-27 change
(patch 0428) is folded into `drivers/display/panel`.

`SCION_XNU_FROM_PATCHES=1` makes the tree the old way (Apple's tag and this repository's patches), and `XNU_FORK_URL` fetches
the fork from elsewhere (a local clone that has the commit).

## Changing the kernel

Commit in the fork, on the profile's branch (or a branch from it, with a pull request there), push, and set the profile's
`fork_commit` to the new commit in a pull request here. A tree made by `xnu/apply.sh` is already a clone of the fork at that
commit: work in it, commit, and push from it. `xnu/export.sh` and the patch series are the old way and go once nothing reads them.
Apple's next release is a merge: `git merge xnu-NEW-TAG` on the branch.

## Proof that nothing changed

When the fork was made (9 November 2026, from this repository at `706528182d10`), the trees were made both ways on the Mac mini
and compared with `diff -r` (content) and the list of executable files: the same, for all three profiles (ios-16.5 with the
S9's configuration and every device repository checked out; ios-26; ios-27 with the Kernel Debug Kit 26.5). The kernels built
from them are compared in the pull request that brought this page.

## What is next

1. Remove what the fork now holds from this repository: `patches/`, `xnu/hakios.patch`, `xnu/files/`, `xnu/files-12377/`,
   `xnu/patches/`, `xnu/hfs/hfs.patch` (the tools and checks that read them first).
2. The drivers into the fork's tree, one source per hardware block with `bases` (`docs/two-bases.md`), deduplicated against the
   copies the ios-27 branch carries. `hakios.dwc3.c`, `hakios.exynos_tmu.h`, `hakios.scion_gov.c` and the files of the old `copy`
   lines are one source already (`profiles/ios-27.copy`); left: `hakios.exynos_cpufreq.c`, `hakios.exynos_tmu.c`, `hakios.exynos_dpu.c`,
   `hakios.exynos_pcie.c` (they differ in behaviour, so merging them changes one kernel or the other), `hakios.s6e3ha8.c` (patch 0428),
   `hakios.exynos_acpm_dvfs.c` (the series' own) and the SoC headers `hakios.exynos_soc.h` and `hakios.exynos9810_mem.h`.
3. One xnu-12377 branch: the stand-ins both machine layers make (SART, UAT, tunables, the read-only region, crypto, AMFI) merged,
   so that `scion/xnu-12377` builds VMAPPLE and STARLTE.
4. The Starbite and wardevsa files renamed to Scion's conventions, and the boards STARLTE does not use (SWEET, A5XELTE, HEROLTE)
   dropped, as commits on the fork.
