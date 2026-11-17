#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build.sh : devshd for iOS (arm64, ad-hoc signed) into jb/ios27/ of the checkout (never in git). Needs Xcode's iPhoneOS SDK.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${JB_ROOT:-$REPO/jb}/ios27
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p "$OUT"
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=15.0 -O2 -Wall -Wextra -Werror \
	-o "$OUT/devshd" "$(dirname "$0")/devshd.c"
codesign -f -s - "$OUT/devshd" 2>/dev/null
[[ "$(lipo -archs "$OUT/devshd")" == arm64 ]]
echo "build.sh: $OUT/devshd"
