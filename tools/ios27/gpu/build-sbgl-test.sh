#!/usr/bin/env bash
# Build tools/ios27/gpu/sbgl-test.c for iPadOS 18 against the iOS Mesa of
# tools/ios27/mesa-ios/mk-mesa-ios.sh, ad-hoc signed:
#   out/ios/sbgl-test, out/ios/libOSMesa.8.dylib (install name
#   /usr/local/starbite/lib/libOSMesa.8.dylib, where the root gets it).
set -euo pipefail
cd "$(dirname "$0")/../../.."
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
M=build/mesa-ios
mkdir -p out/ios
cp $M/build-pan/src/gallium/targets/osmesa/libOSMesa.8.dylib out/ios/libOSMesa.8.dylib
chmod u+w out/ios/libOSMesa.8.dylib
xcrun --sdk iphoneos strip -x out/ios/libOSMesa.8.dylib
install_name_tool -id /usr/local/starbite/lib/libOSMesa.8.dylib out/ios/libOSMesa.8.dylib 2>/dev/null
codesign -f -s - out/ios/libOSMesa.8.dylib
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall \
	-I$M/mesa-25.0.7/include -framework IOKit -framework CoreFoundation \
	tools/ios27/gpu/sbgl-test.c out/ios/libOSMesa.8.dylib -o out/ios/sbgl-test
codesign -f -s - out/ios/sbgl-test
otool -L out/ios/sbgl-test | grep -i osmesa
ls -la out/ios/sbgl-test out/ios/libOSMesa.8.dylib
