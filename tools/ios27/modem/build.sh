#!/bin/sh
# SPDX-License-Identifier: MIT-0
# tools/ios27/modem/build.sh [OUT]: scion-celld (pdp_ip0 as a configd network service) and scion-fetch (a test GET through
# iOS's own stack) for the iOS 27 root -> OUT (default: ./out). arm64 only, signed ad hoc. Needs Xcode's iPhoneOS SDK.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=15.0 -O2 -Wall -Wextra -Werror \
	-framework CoreFoundation -framework SystemConfiguration -o "$OUT/scion-celld" "$HERE/scion-celld.c"
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=15.0 -O2 -Wall -fobjc-arc \
	-framework Foundation -o "$OUT/scion-fetch" "$HERE/scion-fetch.m"
for b in scion-celld scion-fetch; do
	codesign -f -s - "$OUT/$b" 2>/dev/null
	[ "$(lipo -archs "$OUT/$b")" = arm64 ] || { echo "build.sh: $b is not plain arm64" >&2; exit 1; }
done
(cd "$OUT" && shasum -a 256 scion-celld scion-fetch)
# scion-bars: c.dylib, the status bar's cellular entry from the modem (SpringBoard, through sweetinject)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=15.0 -O2 -Wall -fobjc-arc -dynamiclib \
	-install_name /usr/lib/c.dylib -framework Foundation -o "$OUT/c.dylib" "$HERE/scion-bars.m"
codesign -f -s - "$OUT/c.dylib" 2>/dev/null
[ "$(lipo -archs "$OUT/c.dylib")" = arm64 ] || { echo "build.sh: c.dylib is not plain arm64" >&2; exit 1; }
(cd "$OUT" && shasum -a 256 c.dylib)
