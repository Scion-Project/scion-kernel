#!/bin/sh
# SPDX-License-Identifier: MIT-0
# tools/ios27/camera/build.sh [OUT]: the S9's camera for the iOS 27 root -> OUT (default ./cam.dylib): camera27.c (the
# library inserted into cameracaptured) with iOS 16.5's capture device, userland/src/camera_device.c and camera_isp.c, built
# for iOS 27: its formats N104's (the iPhone 11's, HAKIOS_CAMERA_N104), its lines to the console (HAKIOS_CAMERA_CONSOLE),
# its colour tunings, where there are any, from /private/var/hakios/camera (HAKIOS_CAMERA_FW). arm64 only, signed ad hoc,
# as the other inserted libraries of this root. Needs Xcode's iPhoneOS SDK (DEVELOPER_DIR).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../../userland/src
OUT=${1:-$HERE/cam.dylib}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
	-dynamiclib -install_name /usr/lib/cam.dylib \
	-DHAKIOS_CAMERA_N104 -DHAKIOS_CAMERA_CONSOLE '-DHAKIOS_CAMERA_FW="/private/var/hakios/camera"' -I"$SRC" \
	-framework CoreFoundation -framework CoreMedia -framework CoreVideo -lobjc \
	-o "$OUT" "$HERE/camera27.c" "$HERE/camera_trace.c" "$HERE/camera_oslog.c" "$SRC/camera_device.c" "$SRC/camera_isp.c"
codesign -f -s - "$OUT" 2>/dev/null
[ "$(lipo -archs "$OUT")" = arm64 ] || { echo "build.sh: $(basename "$OUT") is not plain arm64" >&2; exit 1; }
shasum -a 256 "$OUT"
