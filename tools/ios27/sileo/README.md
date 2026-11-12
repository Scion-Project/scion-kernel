# tools/ios27/sileo: the package manager on an iOS 27 root

What it is and why: [docs/ios27/SILEO.md](../../../docs/ios27/SILEO.md). The files:

| File | What |
|---|---|
| `prepare.sh` | Fetch and check the Procursus bootstrap and Sileo (`tools/mk-jb.sh`, pinned), build `jb_seed` and `var_sync` for iOS. No root. Output in `jb/` of the checkout |
| `build-tools.sh` | The second half of that: `userland/src/jb_seed.c` and `var_sync.c` with Xcode's iPhoneOS SDK, arm64, ad-hoc signed |
| `../fullroot/steps.d/60-sileo.sh ROOT [undo]` | The root step. Part of every full-root build; also run by hand on an attached root |
| `jobs.py ROOT add [--selftest]\|remove\|list` | The launchd jobs, as entries of `/System/Library/xpc/launchd.plist` |
| `selftest.sh` | Runs on the phone on a test root (`SILEO_SELFTEST=1`), writes `/private/var/scion-test/selftest.log` |

## A new root

    tools/ios27/s9/prepare-root-inputs.sh ...     # calls prepare.sh
    sudo tools/ios27/s9/build-root.sh inputs      # 60-sileo.sh runs with the other steps; check-root.sh checks it

## A phone that already has an iOS 27 root

One reversible root step. The phone is in TWRP; `SERIAL` is its adb serial, `BASE` the image its root was flashed from
(the pull only fetches what differs from it, and `BASE` is not written to). `root-sync` stands for a chunked pull and
push of USERDATA through TWRP's adb, pinned to that serial (the Starbite tools' `starlte-root-sync.py` is the one used
so far); without one, edit a copy of `BASE` and write it whole with `tools/ios27/fullroot/fastflash-root.py`, which
replaces everything on the phone's root.

    tools/ios27/sileo/prepare.sh
    root-sync pull BASE work/root.img                         # the phone's root as it is now (+ work/root.img.hashes)
    hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint work/mnt work/root.img
    tools/ios27/fullroot/steps.d/60-sileo.sh work/mnt         # undo: the same with "undo" as the second argument
    hdiutil detach work/mnt
    root-sync push work/root.img                              # the changed 64 MiB chunks, each read back

Without root on the Mac the new files carry the builder's user ID; `jb_seed` makes them root's at the first boot (it says
how many in `/private/var/log/scion-jbseed.log`). The first boot after the step copies the prefix (about 120 MB, half a
minute after SpringBoard is up), then Sileo's icon appears on the last page of the home screen.

To take it off again: the same three commands with `60-sileo.sh work/mnt undo` (add `PURGE_PREFIX=1` to delete the
phone's own copy and everything installed into it as well).

## Reading the self-test

    root-sync pull BASE work/after.img
    hdiutil attach -readonly -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint work/mnt work/after.img
    cat work/mnt/private/var/scion-test/selftest.log work/mnt/private/var/log/scion-jbseed.log

## Status

| | |
|---|---|
| The step, its undo, a second run, `check-root.sh` | Run on a clone of a full RC2 root image on the Mac: applies, repeats without change, undo leaves `launchd.plist` identical, `fsck_hfs` clean |
| On a phone | Not yet: see the pull request that brought this for what has been seen since |
