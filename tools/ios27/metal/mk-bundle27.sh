#!/bin/sh
# Starbite's tools/metal2/build-starbite-metal.sh for iOS 27: StarbiteMetal.bundle (HotMetal2.mm + NativeBridgeMetalFrontend.mm + the bridge +
# PanVK, arm64, ad hoc signed) and m2vtest -> $W/stage (unstripped copies in $W/sym).
#   SB=<Starbite tree> DEVELOPER_DIR=<Xcode 16.4> tools/ios27/metal/mk-bundle27.sh   (after bridge.sh and mk-panvk27.sh)
set -e
W=${W:-$PWD/build/metal27}; cd "${SB:?set SB to the Starbite tree}"
: "${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode}"; export DEVELOPER_DIR
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0"
BRIDGE=$SB/build/m2v-bridge/ios/libnative_metal_bridge.a
PANVK=$W/build/mesa/build-vk26/src/panfrost/vulkan/libvulkan_panfrost.dylib
B=$W/stage/StarbiteMetal.bundle
rm -rf "$B" && mkdir -p "$B" $W/sym $W/stage
$CC -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-function -bundle \
	-o "$B/StarbiteMetal" tools/metal2/HotMetal2.mm tools/metal2/NativeBridgeMetalFrontend.mm -x none "$BRIDGE" \
	-Wl,-dead_strip -lc++ -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
	-framework CoreFoundation 2>&1 | grep -v "was built for newer 'iOS' version" | grep -E "error|warning: unused" | head -20 || true
[ -f "$B/StarbiteMetal" ] || exit 1
cp "$B/StarbiteMetal" $W/sym/StarbiteMetal && cp "$PANVK" $W/sym/libvulkan_panfrost.dylib
xcrun strip -x "$B/StarbiteMetal"
cp tools/metal2/Info.plist "$B/Info.plist"
cp "$PANVK" "$B/libvulkan_panfrost.dylib"
install_name_tool -id @loader_path/libvulkan_panfrost.dylib "$B/libvulkan_panfrost.dylib" 2>/dev/null
xcrun strip -x "$B/libvulkan_panfrost.dylib"
codesign -f -s - "$B/libvulkan_panfrost.dylib" 2>/dev/null
codesign -f -s - "$B" 2>/dev/null
$CC -O2 -Wall -o $W/stage/m2vtest tools/metal2/m2vtest.c "$BRIDGE" -Wl,-dead_strip -lc++ 2>&1 | grep -E "error" || true
xcrun strip -x $W/stage/m2vtest && codesign -f -s - $W/stage/m2vtest
ls -la "$B" $W/stage
