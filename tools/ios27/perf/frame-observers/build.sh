#!/bin/sh
# SPDX-License-Identifier: MIT-0
# build.sh [OUT]: the two frame observers of docs/ios27/PACING.md (diagnostics, not part of any root): cawake.dylib for backboardd
# (/usr/lib/bd.dylib on the phone, sweetinject ...;/usr/lib/bd.dylib,backboardd) and sbpage.dylib for SpringBoard (/usr/lib/pd.dylib,
# ...;/usr/lib/pd.dylib,SpringBoard). Needs the iPhoneOS SDK (xcrun); nothing of Apple's is copied.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$HERE/out}; mkdir -p "$OUT"
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall -fobjc-arc -dynamiclib"
$CC -install_name /usr/lib/bd.dylib -framework Foundation -framework CoreFoundation "$HERE/cawake.m" -o "$OUT/cawake.dylib"
$CC -install_name /usr/lib/pd.dylib -framework UIKit -framework QuartzCore -framework Foundation -framework CoreFoundation \
	"$HERE/sbpage.m" -o "$OUT/sbpage.dylib"
codesign -f -s - "$OUT/cawake.dylib" "$OUT/sbpage.dylib"
