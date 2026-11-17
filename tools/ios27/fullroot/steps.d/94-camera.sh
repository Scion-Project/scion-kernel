#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 94-camera.sh ROOT [undo] : what iOS 27 needs on the root to use the S9's camera (docs/ios27/CAMERA.md): the capture device
# of iOS 16.5 (userland/src/camera_device.c, camera_isp.c) with its iOS 27 entry (tools/ios27/camera/camera27.c), built here
# with the builder's Xcode (tools/ios27/camera/build.sh) as /usr/lib/cam.dylib, recorded for insertion into cameracaptured
# (the record next to the image; make-boot-image.sh then adds ";/usr/lib/cam.dylib,cameracaptured" to sweetinject=).
# The kernel's side is NOT on by default: the camera's stream needs cam_s9=7 on the boot image's command line (the front
# camera; with cam_rear=1 the rear one). Without it the library does nothing in cameracaptured, and make-boot-image.sh adds no
# /arm-io/isp, so the Camera app stays hidden as before (tools/ios27/camera/adt-isp-node.py).
# Also: MobileGestalt's cache on /var is removed (it is made again at the next boot), so that an answer of an earlier boot
# ("no camera") is not kept; and CAMERA_TUNING_DIR (optional): camera-<sensor>.isp colour tunings you made from your own
# phone's files (tools/camtuning-exynos.py; never in the repository) to /private/var/hakios/camera.
# The trace (docs/ios27/CAMERA.md, "The trace"; off by default): CAMERA_TRACE=N writes /private/var/hakios/camera/trace (the
# library's switch, the same as cam_trace=N on the boot line); CAMERA_TRACE_APP=1 also records the library for Camera.app,
# where it only takes the app's os_log while the trace is on.
#   ENABLE_CAMERA=0  off.   undo: the library, its record, the tunings this step put there and the cache taken off the root.
# Idempotent. NOT run on a phone: neither the library nor this step (the capture device itself streamed into Camera.app on
# iOS 16.5, 3 November 2026).
set -euo pipefail
. "$(cd "$(dirname "$0")/.." && pwd)/feature.sh"
feature_setup 94-camera CAMERA
LIB=/usr/lib/cam.dylib
TUNE=/private/var/hakios/camera
MGCACHE=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist

feature_inputs() {
	feature_have_sdk || feature_skip "no Xcode with the iPhoneOS SDK: set DEVELOPER_DIR"
	[[ -z ${CAMERA_TUNING_DIR:-} || -d $CAMERA_TUNING_DIR ]] || feature_skip "CAMERA_TUNING_DIR=$CAMERA_TUNING_DIR is not a directory"
}

# MobileGestalt's cache: answers of the boot that made it (still-camera among them, it seems); made again when missing
mg_cache_off() {
	if [[ -e $R$MGCACHE ]]; then
		rm -f "$R$MGCACHE"
		feature_say "MobileGestalt's cache removed: it is made again at the next boot, with the camera as that boot has it"
	fi
}

feature_apply() {
	feature_build sh "$FEATURE_SK/tools/ios27/camera/build.sh" "$FEATURE_OUT/cam.dylib" >/dev/null
	[[ $(lipo -archs "$FEATURE_OUT/cam.dylib") == arm64 ]] || feature_die "cam.dylib is not plain arm64"
	feature_put "$FEATURE_OUT/cam.dylib" "$R$LIB" 0755
	local programs=cameracaptured
	[[ ${CAMERA_TRACE_APP:-0} != 1 ]] || programs="cameracaptured Camera"
	feature_inject "$LIB" $programs
	if [[ ${CAMERA_TRACE:-0} =~ ^[1-9]$ ]]; then
		install -d -m 0755 "$R$TUNE"
		printf '%s\n' "$CAMERA_TRACE" > "$R$TUNE/trace"
		chmod 0644 "$R$TUNE/trace"
		feature_say "the camera's trace on (CAMERA_TRACE=$CAMERA_TRACE: $TUNE/trace); the library in: $programs"
	else
		rm -f "$R$TUNE/trace"
	fi
	if [[ -n ${CAMERA_TUNING_DIR:-} ]]; then
		local n=0 f
		for f in "$CAMERA_TUNING_DIR"/camera-*.isp; do
			[[ -f $f ]] || continue
			feature_put "$f" "$R$TUNE/$(basename "$f")" 0644
			n=$((n + 1))
		done
		feature_say "$n colour tuning(s) from CAMERA_TUNING_DIR to $TUNE"
	fi
	mg_cache_off
	feature_say "$LIB built from tools/ios27/camera/camera27.c and userland/src/camera_device.c, camera_isp.c (sha256 $(shasum -a 256 "$R$LIB" | cut -c1-16)), to be inserted into cameracaptured; the kernel's camera needs cam_s9=7 on the boot image's line"
}

feature_undo() {
	feature_uninject "$LIB"
	rm -f "$R$LIB" "$R$TUNE"/camera-*.isp "$R$TUNE/trace"
	rmdir "$R$TUNE" 2>/dev/null || true
	mg_cache_off
	feature_say "undone: $LIB, its record and $TUNE's tunings taken off the root"
}

feature_main "$@"
