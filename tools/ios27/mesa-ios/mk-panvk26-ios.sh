#!/usr/bin/env bash
# PanVK (Mesa 26.2.6's Vulkan driver for Mali, Vulkan 1.2+ for metal2vulkan) for iOS, reaching the
# kernel's StarbiteMali through src/starbite-drm (tools/ios27/mesa-ios/starbite-drm), -> out/ios/libvulkan_panfrost26.dylib.
# Mesa 26 precompiles Panfrost's OpenCL C kernels at build time, so three host tools are built first, natively:
# mesa_clc, vtn_bindgen2 and panfrost_compile (Homebrew: llvm, spirv-llvm-translator, libclc, spirv-tools).
# The cross build reuses mk-mesa-ios.sh's cross.ini and venv (run that once first).
#   tools/ios27/mesa-ios/mk-panvk26-ios.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
HERE=$ROOT/tools/mesa-ios
B=$ROOT/build/mesa-ios
VERSION=26.2.6
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
cd "$B"
if [ ! -d mesa-$VERSION ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	tar xJf mesa-$VERSION.tar.xz
	patch -s -p1 -d mesa-$VERSION < "$HERE/mesa26-panvk-ios.patch"
fi
rm -rf mesa-$VERSION/src/starbite-drm
cp -r "$HERE/starbite-drm" mesa-$VERSION/src/starbite-drm
COMMON="-Dplatforms= -Dgallium-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled
	-Dgles2=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled
	-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false"
# host tools
(
	export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/bison/bin:$B/venv/bin:$PATH
	export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/share/pkgconfig
	[ -f build-clc-host/build.ninja ] || meson setup build-clc-host mesa-$VERSION --buildtype=release $COMMON \
		-Dvulkan-drivers=panfrost -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled \
		-Dllvm=enabled -Dshared-llvm=enabled > meson-clc-host.log
	ninja -C build-clc-host src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
		src/panfrost/clc/panfrost_compile > ninja-clc-host.log
)
# PanVK for iOS
export PATH=$B/build-clc-host/src/compiler/clc:$B/build-clc-host/src/compiler/spirv:$B/build-clc-host/src/panfrost/clc:/opt/homebrew/opt/bison/bin:$B/venv/bin:$PATH
[ -f build-vk26/build.ninja ] || meson setup build-vk26 mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local/starbite --libdir=lib --buildtype=release -Db_ndebug=true $COMMON -Dvulkan-drivers=panfrost \
	-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dshader-cache=disabled -Dlmsensors=disabled > meson-vk26.log
ninja -C build-vk26 > ninja-vk26.log
cp build-vk26/src/panfrost/vulkan/libvulkan_panfrost.dylib "$ROOT/out/ios/libvulkan_panfrost26.dylib"
install_name_tool -id /usr/local/starbite/lib/libvulkan_panfrost.dylib "$ROOT/out/ios/libvulkan_panfrost26.dylib"
codesign -f -s - "$ROOT/out/ios/libvulkan_panfrost26.dylib"
echo "mk-panvk26-ios.sh: $ROOT/out/ios/libvulkan_panfrost26.dylib"
