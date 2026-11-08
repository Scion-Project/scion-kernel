#!/usr/bin/env bash
# Build tools/ios27/gpu/sbgpu-test.c for iPadOS 18 (arm64, iPhoneOS SDK of Xcode 16.4),
# ad-hoc signed: out/ios/sbgpu-test.
set -euo pipefail
cd "$(dirname "$0")/../../.."
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
mkdir -p out/ios
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall \
	-framework IOKit -framework CoreFoundation tools/ios27/gpu/sbgpu-test.c -o out/ios/sbgpu-test
codesign -f -s - out/ios/sbgpu-test
file out/ios/sbgpu-test
