#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 90-sensors.sh ROOT [undo] : the device's orientation from the S9's accelerometer for iOS (docs/ios27/SENSORS.md): the
# library tools/ios27/sensors/orientation.c, built here with the builder's Xcode (tools/ios27/sensors/build.sh), as
# /usr/lib/o.dylib, recorded for insertion into backboardd (the record next to the image; make-boot-image.sh then adds
# ";/usr/lib/o.dylib,backboardd" to sweetinject=). The kernel's side (shub=6 shub_hid=3) is on by default.
#   ENABLE_SENSORS=0  off.   undo: the library and its record taken off the root.
# With the Metal compositor a turned interface is drawn by the bundle's "turned textures" (tools/ios27/metal/patches/
# frontend-turned.diff, applied by tools/ios27/metal/mk-bundle27.sh unless NOPATCH=1; docs/ios27/METAL-ROTATION.md); a
# bundle built without it keeps the panel's last picture while the interface is turned.
# Idempotent. The library has been run on a phone (12 November 2026: a turn drawn under the OpenGL ES compositor, and under
# the Metal compositor with turned textures); this step has NOT been run on a phone.
set -euo pipefail
. "$(cd "$(dirname "$0")/.." && pwd)/feature.sh"
feature_setup 90-sensors SENSORS
LIB=/usr/lib/o.dylib

feature_inputs() {
	feature_have_sdk || feature_skip "no Xcode with the iPhoneOS SDK: set DEVELOPER_DIR"
}

feature_apply() {
	feature_build sh "$FEATURE_SK/tools/ios27/sensors/build.sh" "$FEATURE_OUT/o.dylib" >/dev/null
	[[ $(lipo -archs "$FEATURE_OUT/o.dylib") == arm64 ]] || feature_die "o.dylib is not plain arm64"
	feature_put "$FEATURE_OUT/o.dylib" "$R$LIB" 0755
	feature_inject "$LIB" backboardd
	feature_say "$LIB built from tools/ios27/sensors/orientation.c (sha256 $(shasum -a 256 "$R$LIB" | cut -c1-16)), to be inserted into backboardd"
}

feature_undo() {
	feature_uninject "$LIB"
	rm -f "$R$LIB"
	feature_say "undone: $LIB and its record taken off the root"
}

feature_main "$@"
