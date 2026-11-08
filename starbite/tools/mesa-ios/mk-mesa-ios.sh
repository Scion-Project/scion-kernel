#!/usr/bin/env bash
# Build Mesa 25.0.7 for iPadOS 18 (arm64-apple-ios, Xcode 16.4's iPhoneOS SDK):
# softpipe + Panfrost (the Mali-G72) in one libOSMesa.8.dylib. Panfrost
# reaches the kernel's StarbiteMali user client through starbite-drm/, a
# stand-in for libdrm. After HakiOS tools/mk-mesa.sh (MIT); the OSMesa ES
# profile part of panfrost-ios.patch is HakiOS's osmesa-es-profile.patch.
#   tools/mesa-ios/mk-mesa-ios.sh -> build/mesa-ios/build-pan/.../libOSMesa.8.dylib
# Needs: python3 (meson, mako, pyyaml, packaging go into a venv), ninja,
# Homebrew bison (macOS's 2.3 is too old), flex.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/tools/mesa-ios
B=$ROOT/build/mesa-ios
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
mkdir -p "$B"
cd "$B"
stamp=$(cat "$HERE"/*.patch | shasum -a 256 | cut -c1-16)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.starbite-patches 2>/dev/null)" != "$stamp" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf mesa-$VERSION build-pan
	tar xJf mesa-$VERSION.tar.xz
	for p in "$HERE"/*.patch; do patch -s -p1 -d mesa-$VERSION < "$p"; done
	echo "$stamp" > mesa-$VERSION/.starbite-patches
fi
rm -rf mesa-$VERSION/src/starbite-drm
cp -r "$HERE/starbite-drm" mesa-$VERSION/src/starbite-drm
if [ ! -x venv/bin/meson ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson mako pyyaml packaging
fi
export PATH=/opt/homebrew/opt/bison/bin:$B/venv/bin:$PATH
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
cpp = ['$CXX', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
ar = '$(xcrun --sdk iphoneos -f ar)'
ranlib = '$(xcrun --sdk iphoneos -f ranlib)'
strip = '$(xcrun --sdk iphoneos -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
[ -f build-pan/build.ninja ] || meson setup build-pan mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local/starbite --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C build-pan > ninja.log
echo "mk-mesa-ios.sh: $B/build-pan/src/gallium/targets/osmesa/libOSMesa.8.dylib"

# VULKAN=1: PanVK too (panvk-ios.patch; Bifrost v6 needs PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 at run
# time), -> out/ios/libvulkan_panfrost.dylib with the install name it has on the root.
if [ "${VULKAN:-0}" = 1 ]; then
	[ -f build-vk/build.ninja ] || meson setup build-vk mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
		--prefix=/usr/local/starbite --libdir=lib --buildtype=release -Db_ndebug=true -Dplatforms= \
		-Dgallium-drivers= -Dvulkan-drivers=panfrost -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
		-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
		-Dtools= -Dlmsensors=disabled -Dbuild-tests=false > meson-vk.log
	ninja -C build-vk > ninja-vk.log
	cp build-vk/src/panfrost/vulkan/libvulkan_panfrost.dylib "$ROOT/out/ios/"
	install_name_tool -id /usr/local/starbite/lib/libvulkan_panfrost.dylib "$ROOT/out/ios/libvulkan_panfrost.dylib"
	codesign -f -s - "$ROOT/out/ios/libvulkan_panfrost.dylib"
	echo "mk-mesa-ios.sh: $ROOT/out/ios/libvulkan_panfrost.dylib"
fi
