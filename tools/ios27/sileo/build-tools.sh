#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-tools.sh : the two programs an iOS 27 root needs for the package manager's prefix, built from the 16.5 line's
# own sources for iOS (arm64, the baseline instruction set: nothing the S9's cores lack), ad-hoc signed:
#   jb_seed   userland/src/jb_seed.c   copies the image's /jb to /private/var/jb once, prepares it at each boot
#   var_sync  userland/src/var_sync.c  the kept volume to the flash every few seconds (the phone is reset, never shut down)
# Into jb/ios27/ (never in the repository, like the rest of jb/). Needs Xcode's iPhoneOS SDK. No root.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${JB_ROOT:-$REPO/jb}/ios27
SDK=$(xcrun --sdk iphoneos --show-sdk-path) || { echo "build-tools.sh: no iPhoneOS SDK (Xcode)" >&2; exit 1; }
mkdir -p "$OUT"
for tool in jb_seed var_sync; do
	xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=15.0 -O2 -Wall -Wextra -Werror \
		-o "$OUT/$tool" "$REPO/userland/src/$tool.c"
	codesign -f -s - "$OUT/$tool" 2>/dev/null
	# arm64 and nothing else: an arm64e slice would need the PAC rewrite the rest of the root went through
	[[ "$(lipo -archs "$OUT/$tool")" == arm64 ]] || { echo "build-tools.sh: $tool is not plain arm64" >&2; exit 1; }
done
(cd "$OUT" && shasum -a 256 jb_seed var_sync > SHA256SUMS)
echo "build-tools.sh: $OUT/jb_seed, $OUT/var_sync"
