#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-root.sh [INPUTS_DIR] : the iOS 27 root image for the Galaxy S9's USERDATA from your own inputs, in one command (as root:
# it has to keep the files' owners). It is tools/ios27/fullroot/build-full-root.sh with the names fetch-apple-inputs.sh gave the
# three images and the directory prepare-root-inputs.sh made.
#   sudo tools/ios27/s9/build-root.sh inputs     -> tools/ios27/fullroot/out/root-full.img (raw HFSX, 32 GiB sparse, about 20 GiB used)
#                                                   and root-full.img.inject beside it
# Environment, all optional: PY=<python with numpy>; SIZE_GB (32); OUT=<image>; MIN_FREE_GB (62: the copy, the cache and working
# room); DAEMON_SET=bp|min; METAL_CA_CLIENTS (default here: "backboardd SBRendererService", what the icon fixes were tested with);
# ENABLE_SILEO=0 for a root without the package manager (docs/ios27/SILEO.md);
# ENABLE_METAL_CA=0 for the OpenGL ES compositor (then boot with tools/ios27/cmdline/starlte-rc-gl.txt).
# The S9's features, each on by default and built here with your Xcode (pass DEVELOPER_DIR through sudo), each skipped with one
# line when what only you can supply is missing, ENABLE_<NAME>=0 to leave one out (tools/ios27/fullroot/steps.d/9N-*.sh):
#   ENABLE_SENSORS    rotation (o.dylib in backboardd)
#   ENABLE_WIFI       Wi-Fi: needs WIFI_FW_DIR=<your phone's own Wi-Fi files>; WIFI_WIFID=0 leaves wifid out
#   ENABLE_AUDIO_BT   speakers, microphone, Bluetooth: needs HCD=<your phone's own bcm4361B2_*.hcd> and PY with numpy
#   ENABLE_MODEM      the modem's network, bars and data (c.dylib in SpringBoard); data needs cp_apn= on the boot image's line
#   ENABLE_CAMERA     the camera (cam.dylib in cameracaptured; docs/ios27/CAMERA.md, NOT run on a phone); needs cam_s9=7 on the
#                     boot image's line, and then make-boot-image.sh shows the Camera app
#   STRICT=1          a feature that fails stops the build (otherwise it is undone and the build goes on without it)
# Next to the image: root-full.img.inject, the libraries to insert, which make-boot-image.sh reads: make the boot image again
# after the root. About 40 minutes.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); FR=$(cd "$HERE/../fullroot" && pwd)
IN=$(cd "${1:-inputs}" && pwd)
for f in os.dmg systemos.dmg appos.dmg; do [ -f "$IN/$f" ] || { echo "build-root: no $IN/$f (tools/ios27/s9/fetch-apple-inputs.sh)" >&2; exit 1; }; done
export IOS27_PRIVATE=${IOS27_PRIVATE:-$FR/private}
for f in herolte-tune-ios-root.sh patches.txt undef.txt overlay/MANIFEST.sha256; do
	[ -f "$IOS27_PRIVATE/$f" ] || { echo "build-root: no $IOS27_PRIVATE/$f (tools/ios27/s9/prepare-root-inputs.sh)" >&2; exit 1; }
done
export OS_DMG=$IN/os.dmg SYS_DMG=$IN/systemos.dmg APP_DMG=$IN/appos.dmg
export METAL_CA_CLIENTS=${METAL_CA_CLIENTS:-backboardd SBRendererService}
if [ -f "$IOS27_PRIVATE/metal-ca-files/MANIFEST.sha256" ]; then
	export METAL_CA_FILES=$IOS27_PRIVATE/metal-ca-files
else
	echo "build-root: no Metal bundle was prepared: OpenGL ES compositor (boot with tools/ios27/cmdline/starlte-rc-gl.txt)"
	export ENABLE_METAL_CA=0
fi
exec "$FR/build-full-root.sh" build
