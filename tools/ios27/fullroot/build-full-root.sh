#!/bin/bash
# build-full-root.sh — iOS 27.0.1 (24A446, N104AP) IPSW images -> FULL, flashable root for the Galaxy S9 (starlte).   mini-side tool.
#
#   sudo ./build-full-root.sh build                 # IPSW -> out/root-full.img (raw HFSX, volume at byte 0; sparse file)
#   ./build-full-root.sh check MOUNTED_ROOT         # offline checks of a mounted root
#   sudo ./build-full-root.sh verify IMG            # fsck + attach read-only + offline checks of a built image
#   ./build-full-root.sh flash SERIAL IMG [BOOT]    # TWRP only; SERIAL is REQUIRED and pinned in every adb call
#
# What "full" means: every file of the OS image and of the SystemOS + AppOS cryptexes, nothing trimmed (no lproj/espresso/mlmodelc/
# TTS/LinguisticData/Health/... filtering, all stock apps installed into /Applications); only the two 1.2 GB dyld_shared_cache *.symbols
# files are left out. On top, the owner build's root edits, each one an idempotent step (tools/steps.d/NN-*.sh ROOT; peers drop new
# NN-*.sh files there, they run in name order):
#   cache (tools/pipeline/cache-pipeline.sh)  a64e + LSE + corecrypto PAC + PACGA/LDRA rewrites, Settings / kbd / QuartzCore-clamp /
#                                             PosterBoard Fix A cache patches, full AOT rewrite (rewrite-grow -> fix-cs -> grow-check)
#   10 macho-a64e   PAC rewrite of every arm64e Mach-O outside the caches
#   20 launchd      hardware + crash-loop daemons only (lists/), _PanicOnCrash dropped, launchctl overrides   [DISABLE_HOGS=1 = opt-in CPU-hog list]
#                   [CC_CONTROLS=1 / CRASH_REPORTS=1 / KEEP_JOBS="LABEL ..." = jobs left on that the set would remove; all off]
#   30 overlay      sbca/m/b/s dylibs (brightness shim = b.dylib), libOSMesa/libsbgl, StarbiteMetal, cc_fips_test stand-in, wallpaper layers
#   40 prefs        Reduce Motion / Transparency OFF, CA compression off, watchdogs off
#   50 xpc-disable  MessagesBlastDoorService
#   60 sileo        the package manager (ENABLE_SILEO=0: out)
#   90 sensors  91 wifi  92 audio-bt  93 modem  94 camera   what iOS needs to see the S9's hardware the kernel turns on by default, each
#                   built from this checkout with the builder's Xcode (DEVELOPER_DIR), each skipped with one line when an input
#                   only the builder has is missing (an Xcode), ENABLE_SENSORS/WIFI/AUDIO_BT/MODEM/CAMERA=0 to leave one out;
#                   a feature that fails is undone and the build goes on (STRICT=1: it stops). feature.sh has the rules.
# The S9's vendor firmware (firmware.sh), FIRST, before anything is copied: devices/starlte/tools/fetch-firmware.sh makes
#   devices/starlte/firmware/ complete (FIRMWARE_FROM=DIR: your own copy, else the public trees it pins; FIRMWARE_OFFLINE=1)
#   and every file is checked against its sum; a file missing or wrong stops the build. 91 puts the Wi-Fi's on the root, 92 both
#   Bluetooth patches, make-boot-image.sh the sensor hub's into the boot image. At the end one line per kind says what the
#   root has; a kind a feature that is on needs and the root lacks makes the offline checks FAIL. ENABLE_WIFI=0
#   ENABLE_AUDIO_BT=0 build without both (and without the firmware).
# After the steps, OUT.inject: the libraries the root has for the kernel to insert (inject-record.sh), which
# tools/ios27/s9/make-boot-image.sh puts into the boot image's sweetinject= (docs/ios27/INSTALL-S9.md).
# NOT applied (owner wants the real iOS 27): the 85 "speed" daemon trims, Spotlight/Siri/photos/ML/intelligence off-switches.
# The iOS build: BUILD_VERSION (build-root.sh sets it from the inputs), else IOS27_BUILD, else tools/ios27/builds/DEFAULT; the OS
#   image's own SystemVersion.plist, the private directory's BUILD (prepare-root-inputs.sh) and a reused cache's DONE must all
#   name that build, or the build stops before the cache (docs/ios27/UPDATING-IOS.md). The three images' default names: its .conf.
# One build at a time: a second build with the same work directory (this checkout's work/) or the same OUT is refused while the
#   first runs (build-lock.sh; a lock left by a build that is gone is taken over).
# Env: SIZE_GB=32  OUT=out/root-full.img  STRICT=1 (a feature step that fails stops the build)  REUSE_CACHE=1 (use work/cache if its DONE marker exists)  DISABLE_HOGS=1  KEEP_MOUNTED=1
# DEVICE=frankel (the Pixel 10, Tensor G5; docs/ios27/PIXEL10.md; NOT TESTED: no root has been built or booted for it): its cores
#   run arm64e and every ARMv8.x instruction the cache uses (Cortex-A520/A725/X4 TRMs: APA3 5, GPA3 1, LSE, FP16, LRCPC2 ...),
#   so the stock cache is staged unrewritten (no cache pipeline) and the steps that only undo or repair the rewrite
#   (lists/skip-frankel.list: 10 macho-a64e, 85 textdata-restore) and the S9's Mali GPU bundle (80 metal-ca) are skipped; the
#   overlay is $IOS27_PRIVATE/overlay-frankel (no Mali GL/Metal pieces). `flash` refuses: there is no flashing tool for a Pixel here.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
PIPE=${IOS27_PIPELINE:-$HERE/pipeline}; export IOS27_PIPELINE=$PIPE   # cache pipeline + stage script (public part; private pieces via IOS27_PRIVATE)
export IOS27_PRIVATE=${IOS27_PRIVATE:-$HERE/private}   # overlay/, patches.txt, oracle/ (Apple-derived; never in the repo)
cmd=${1:-}; shift || true
ADB=${ADB:-adb}
PY=${PY:-python3}; export PY
IPSW=${IPSW:-$HERE/ipsw}
DEVICE=${DEVICE:-starlte}
case $DEVICE in starlte|frankel) ;; *) echo "DEVICE=starlte or frankel" >&2; exit 2;; esac
[[ $DEVICE == frankel ]] && export OVERLAY=${OVERLAY:-$IOS27_PRIVATE/overlay-frankel}
. "$HERE/../builds/build-conf.sh"
BUILD_VERSION=${BUILD_VERSION:-${IOS27_BUILD:-$(ios27_build_default)}}
OS_DMG=${OS_DMG:-$IPSW/$(ios27_build_get "$BUILD_VERSION" OS_DMG || true)}
SYS_DMG=${SYS_DMG:-$IPSW/$(ios27_build_get "$BUILD_VERSION" SYSTEMOS_DMG || true)}
APP_DMG=${APP_DMG:-$IPSW/$(ios27_build_get "$BUILD_VERSION" APPOS_DMG || true)}
SIZE_GB=${SIZE_GB:-32}; OUT=${OUT:-$HERE/out/root-full.img}; W=$HERE/work
log() { echo "[$(date -u +%T)] $*"; }

detach_if() { [[ -n ${1:-} ]] && { umount "$1" 2>/dev/null || true; hdiutil detach "$1" >/dev/null 2>&1 || hdiutil detach -force "$1" >/dev/null 2>&1 || true; }; }
attach_src() { # attach_src DMG MOUNTPOINT : read-only, owners on, raw APFS image
    hdiutil attach -readonly -nobrowse -owners on -imagekey diskimage-class=CRawDiskImage -mountpoint "$2" "$1" >/dev/null
}

case $cmd in
build)
    [[ ${EUID} == 0 ]] || { echo "run as root: sudo $0 build" >&2; exit 2; }
    OWNER=${SUDO_USER:?run via sudo}
    # ---- one build at a time in this work directory and into this image (build-lock.sh): two at once spoil both ----
    . "$HERE/build-lock.sh"
    mkdir -p "$W" "$(dirname "$OUT")"
    build_lock_take "$W/build.lock" "the work directory" || exit 1
    build_lock_take "$OUT.lock" "the image $OUT" || { build_lock_release "$W/build.lock"; exit 1; }
    trap 'build_lock_release "$OUT.lock"; build_lock_release "$W/build.lock"' EXIT
    ios27_build_check "$BUILD_VERSION" || exit 1
    B_PRIV=; [[ ! -f $IOS27_PRIVATE/BUILD ]] || read -r B_PRIV < "$IOS27_PRIVATE/BUILD" || true
    ios27_build_same "the private directory ($IOS27_PRIVATE/BUILD)" "$B_PRIV" "this build (BUILD_VERSION)" "$BUILD_VERSION" || exit 1
    for f in "$OS_DMG" "$SYS_DMG" "$APP_DMG"; do [[ -f $f ]] || { echo "missing $f" >&2; exit 1; }; done
    # a quarantined image is mounted with the quarantine option, and rsync then fails on its files' extended attributes
    # half way through staging (macOS 27's openrsync): stop now, before anything is copied
    for f in "$OS_DMG" "$SYS_DMG" "$APP_DMG"; do
        ! xattr -p com.apple.quarantine "$f" >/dev/null 2>&1 || { echo "$f carries com.apple.quarantine (from the download): xattr -d com.apple.quarantine $f" >&2; exit 1; }
    done
    # ---- 0. the S9's vendor firmware, before anything is copied: complete and checked, or no build (firmware.sh) ----
    . "$HERE/firmware.sh"
    if [[ $DEVICE == starlte && ( ${ENABLE_WIFI:-1} != 0 || ${ENABLE_AUDIO_BT:-1} != 0 ) ]]; then
        log "firmware (firmware.sh)"
        fw_prepare || { echo "the S9's firmware is not complete (above): no root is built without it" >&2; exit 1; }
    fi
    free_gb=$(df -g "$HERE" | awk 'NR==2 {print $4}'); need=${MIN_FREE_GB:-62}
    (( free_gb >= need )) || { echo "only $free_gb GB free, need >= $need (keeps 40 GB free after the ~22 GB root + 6 GB cache)" >&2; exit 1; }
    mkdir -p "$W" "$(dirname "$OUT")"; chown "$OWNER" "$W" "$(dirname "$OUT")"
    rm -f "$OUT.inject" "$W/inject.record"   # a record is only ever next to the image it was made from
    # ---- 1. cache (unprivileged) ----
    SYSMNT=$W/mnt-sys; OSMNT=$W/mnt-os; APPMNT=$W/mnt-app; ROOTMNT=$W/mnt-root
    mkdir -p "$SYSMNT" "$OSMNT" "$APPMNT" "$ROOTMNT"
    for m in "$ROOTMNT" "$OSMNT" "$SYSMNT" "$APPMNT"; do detach_if "$m"; done
    attach_src "$OS_DMG" "$OSMNT"; attach_src "$SYS_DMG" "$SYSMNT"; attach_src "$APP_DMG" "$APPMNT"
    cleanup() { detach_if "${ROOTMNT:-}"; [[ -n ${RDEV:-} ]] && hdiutil detach "$RDEV" >/dev/null 2>&1 || true; detach_if "$OSMNT"; detach_if "$SYSMNT"; detach_if "$APPMNT"; build_lock_release "$OUT.lock"; build_lock_release "$W/build.lock"; }
    trap cleanup EXIT
    ios27_build_same "the OS image ($OS_DMG)" "$(ios27_build_of_root "$OSMNT")" "this build (BUILD_VERSION)" "$BUILD_VERSION" || exit 1
    if [[ $DEVICE == frankel ]]; then log "frankel: the stock cache, not rewritten"; else
    B_CACHE=; [[ ! -f $W/cache/DONE ]] || read -r B_CACHE < "$W/cache/DONE" || true
    if [[ ${REUSE_CACHE:-0} == 1 && -f $W/cache/DONE ]]; then
        ios27_build_same "the cache to reuse ($W/cache/DONE)" "$B_CACHE" "this build (BUILD_VERSION)" "$BUILD_VERSION" || exit 1
        log "reusing $W/cache${B_CACHE:+ (build $B_CACHE)}"; else
        log "cache pipeline (a64e, LSE, corecrypto, PACGA, patches, full AOT rewrite)"
        sudo -u "$OWNER" -H env PY="$PY" IOS27_PRIVATE="$IOS27_PRIVATE" "$PIPE/cache-pipeline.sh" "$SYSMNT/System/Library/Caches/com.apple.dyld" "$W/cache" 2>&1 | tee "$W/cache-pipeline.log"
        grep -q CACHE-PIPELINE-DONE "$W/cache-pipeline.log" || { echo "cache pipeline failed" >&2; exit 1; }
        sudo -u "$OWNER" sh -c 'echo "$1" > "$2"' _ "$BUILD_VERSION" "$W/cache/DONE"   # the build it was made from (REUSE_CACHE checks it)
    fi
    fi
    # ---- 2. blank HFSX volume, byte 0 of USERDATA ----
    log "creating ${SIZE_GB} GiB sparse raw image $OUT"
    rm -f "$OUT"; mkfile -n "${SIZE_GB}g" "$OUT"
    RDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
    newfs_hfs -s -J -b 4096 -v IOSROOT "/dev/r${RDEV#/dev/}" >/dev/null
    mount -t hfs -o owners "$RDEV" "$ROOTMNT"
    # ---- 3. stage: full OS + cryptexes, all apps, rewritten cache ----
    log "staging (this copies ~22 GB)"
    CACHE_SRC=$([[ $DEVICE == frankel ]] || echo "$W/cache/c") STAGE_ALL_APPS=1 INSTALL_STAGED_APPS=1 bash "$PIPE/root/stage-full-root.sh" "$OSMNT" "$SYSMNT" "$APPMNT" "$ROOTMNT"
    log "herolte tune (setup done, CommCenter out, autorun job, containers skeleton)"
    bash "$IOS27_PRIVATE/herolte-tune-ios-root.sh" "$ROOTMNT" "$BUILD_VERSION"
    n=0; for app in "$ROOTMNT"/private/var/staged_system_apps/*.app; do [[ -d $app && -d $ROOTMNT/Applications/$(basename "$app") ]] || continue; rm -rf "$app"; n=$((n+1)); done
    echo "staged_system_apps: removed $n copies already in /Applications"
    # ---- 4. steps ----
    for s in "$HERE"/steps.d/[0-9][0-9]-*.sh; do
        if [[ $DEVICE == frankel ]] && grep -qx "$(basename "$s")" "$HERE/lists/skip-frankel.list"; then log "step $(basename "$s"): skipped on frankel"; continue; fi
        log "step $(basename "$s")"; OS_MNT=$OSMNT bash "$s" "$ROOTMNT"
    done
    bash "$HERE/inject-record.sh" "$ROOTMNT" "$W/inject.record"
    sync
    log "offline checks"; chk=0; bash "$HERE/check-root.sh" "$ROOTMNT" | tee "$W/check-root.log" || true; grep -q "ALL HARD CHECKS PASSED" "$W/check-root.log" || chk=1
    fwok=0; [[ $DEVICE != starlte ]] || { fw_summary "$ROOTMNT" > "$W/firmware.log" || { fwok=1; chk=1; }; }
    df -h "$ROOTMNT"; sync
    umount "$ROOTMNT"; log "fsck_hfs"; fsck_hfs -n -f "/dev/r${RDEV#/dev/}" 2>&1 | tail -4 | tee -a "$W/check-root.log"
    hdiutil detach "$RDEV" >/dev/null; RDEV=
    chown "$OWNER" "$OUT"; chmod 644 "$OUT"; ls -ls "$OUT"; shasum -a 256 "$OUT" | tee "$OUT.sha256"
    mv -f "$W/inject.record" "$OUT.inject"; chown "$OWNER" "$OUT.inject"; chmod 644 "$OUT.inject"
    log "sweetinject record $OUT.inject (make-boot-image.sh reads it): $(grep -v '^#' "$OUT.inject" | tr '\n' ';')"
    [[ ! -s $W/firmware.log ]] || { cat "$W/firmware.log"; cat "$W/firmware.log" >> "$W/check-root.log"; }
    [[ $fwok == 0 ]] || log "a feature that is on has no firmware on the root (firmware: ... ABSENT above)"
    log "BUILD-DONE $OUT (offline checks: $([[ $chk == 0 ]] && echo passed || echo FAILED))"; exit $chk
    ;;
verify)
    IMG=${1:?usage: verify IMG}; [[ ${EUID} == 0 ]] || { echo "run as root: sudo $0 verify IMG" >&2; exit 2; }
    mkdir -p "$W/mnt-verify"; VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" | awk 'NR==1 {print $1}')
    trap 'umount "$W/mnt-verify" 2>/dev/null; hdiutil detach "$VDEV" >/dev/null 2>&1' EXIT
    fsck_hfs -n -f "/dev/r${VDEV#/dev/}" 2>&1 | tail -3
    mount -t hfs -o owners,rdonly "$VDEV" "$W/mnt-verify"; bash "$HERE/check-root.sh" "$W/mnt-verify" | tee "$W/check-verify.log"
    ;;
check)
    exec bash "$HERE/check-root.sh" "${1:?mounted root}"
    ;;
flash)
    [[ $DEVICE == starlte ]] || { echo "flash: the Galaxy S9 (TWRP) only; nothing here writes to a Pixel" >&2; exit 2; }
    SERIAL=${1:?usage: flash SERIAL IMG [BOOT]}; IMG=${2:?IMG}; BOOT=${3:-}
    export ANDROID_SERIAL=$SERIAL
    A="$ADB -s $SERIAL"
    [[ $($A get-state 2>/dev/null) == recovery ]] || { echo "$SERIAL is not in TWRP" >&2; exit 1; }
    [[ $($A get-serialno) == "$SERIAL" ]] || { echo "wrong serial" >&2; exit 1; }
    pin=$(mktemp /tmp/adbpin.XXXX); printf '#!/bin/sh\ncase "$1" in kill-server|start-server) exit 0;; esac\nexec %s -s %s "$@"\n' "$ADB" "$SERIAL" > "$pin"; chmod +x "$pin"
    python3 "${FASTFLASH:-$HERE/fastflash-root.py}" "$pin" "$IMG" "${IMG}.flash-$SERIAL.state"
    if [[ -n $BOOT ]]; then
        N=$(stat -f %z "$BOOT"); L=$(shasum -a 256 "$BOOT" | cut -c1-64)
        $A push "$BOOT" /tmp/boot.img >/dev/null; [[ $($A shell sha256sum /tmp/boot.img | cut -c1-64) == "$L" ]] || { echo "push mismatch" >&2; exit 1; }
        $A shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 2>/dev/null; sync"
        [[ $($A shell "head -c $N /dev/block/by-name/BOOT | sha256sum" | cut -c1-64) == "$L" ]] || { echo "BOOT read-back mismatch" >&2; exit 1; }
        echo "FLASH-VERIFIED $L"
    fi
    rm -f "$pin"; echo "flashed (not rebooted): $A reboot"
    ;;
*) sed -n 2,22p "$0"; exit 2;;
esac
