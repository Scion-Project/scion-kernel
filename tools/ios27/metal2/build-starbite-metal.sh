#!/bin/sh
# Build StarbiteMetal.bundle (IOS-26-Hot's HotMetal2 frontend, MIT, over the metal2vulkan bridge and PanVK,
# Mesa 26.2.6's Vulkan driver for the Mali-G72) for iOS arm64, signed ad hoc, into out/ios; and m2vtest, the
# bridge's self-test (device, compute, render, host-memory import, then every QuartzCore shader).
#   tools/ios27/metal2/build-starbite-metal.sh
# First: tools/ios27/metal2/build-m2v-bridge.sh, and PanVK (build/mesa-ios/build-vk26). Starbite's own script, kept as it was
# (paths aside); for iOS 27 use tools/ios27/metal/mk-bundle27.sh, which takes its PanVK from tools/ios27/metal/mk-panvk27.sh.
set -e
cd "$(dirname "$0")/../../.."
ROOT=$PWD
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0"
BRIDGE=$ROOT/build/m2v-bridge/ios/libnative_metal_bridge.a
PANVK=$ROOT/build/mesa-ios/build-vk26/src/panfrost/vulkan/libvulkan_panfrost.dylib
B=out/ios/StarbiteMetal.bundle
rm -rf "$B" && mkdir -p "$B"
$CC -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-function -bundle \
	-o "$B/StarbiteMetal" tools/ios27/metal2/HotMetal2.mm tools/ios27/metal2/NativeBridgeMetalFrontend.mm -x none "$BRIDGE" \
	-Wl,-dead_strip -lc++ -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
	-framework CoreFoundation 2>&1 | grep -v "was built for newer 'iOS' version" || true
[ -f "$B/StarbiteMetal" ] || exit 1
mkdir -p out/ios/sym && cp "$B/StarbiteMetal" out/ios/sym/StarbiteMetal && cp "$PANVK" out/ios/sym/libvulkan_panfrost.dylib
xcrun strip -x "$B/StarbiteMetal"
cp tools/ios27/metal2/Info.plist "$B/Info.plist"
cp "$PANVK" "$B/libvulkan_panfrost.dylib"
install_name_tool -id @loader_path/libvulkan_panfrost.dylib "$B/libvulkan_panfrost.dylib" 2>/dev/null
xcrun strip -x "$B/libvulkan_panfrost.dylib"
codesign -f -s - "$B/libvulkan_panfrost.dylib" 2>/dev/null
codesign -f -s - "$B" 2>/dev/null
$CC -O2 -Wall -o out/ios/m2vtest tools/ios27/metal2/m2vtest.c "$BRIDGE" -Wl,-dead_strip -lc++ 2>&1 | grep -v "was built for newer" || true
xcrun strip -x out/ios/m2vtest && codesign -f -s - out/ios/m2vtest
ls -la "$B" out/ios/m2vtest
