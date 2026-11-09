#!/usr/bin/env bash
# tools/ios27/mesa-ios/mk-panvk26-ios.sh with its paths taken from the environment, for the iOS 27 Metal stack.
# Mesa 26.2.6 + mesa26-panvk-ios / shader-size-cap / bifrost-ra-multispill patches + starbite-drm -> PanVK for iOS arm64.
set -euo pipefail
# Needs Mesa 26.2.6 unpacked in $B/mesa-26.2.6 with tools/ios27/mesa-ios/mesa26-*.patch applied and tools/ios27/mesa-ios/starbite-drm copied to
# its src/starbite-drm (as mk-panvk26-ios.sh does), build-host.sh's $W/host, and a python venv with mako, pyyaml and meson in $W/venv.
#   DEVELOPER_DIR=<Xcode 16.4> [W=...] [IOS_SDK=<iPhoneOS18.5.sdk>] tools/ios27/metal/mk-panvk27.sh
W=${W:-$PWD/build/metal27}; B=$W/build/mesa; M=$B/mesa-26.2.6
: "${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode}"
IOS_SDK=${IOS_SDK:-$DEVELOPER_DIR/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS18.5.sdk}
TC=$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin
mkdir -p $B
cat > $W/cross.ini <<INI
[binaries]
c = ['$TC/clang', '-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=16.0']
cpp = ['$TC/clang++', '-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=16.0']
objc = ['$TC/clang', '-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=16.0']
ar = '$TC/ar'
ranlib = '$TC/ranlib'
strip = '$TC/strip'
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
COMMON="-Dplatforms= -Dgallium-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled
	-Dgles2=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled
	-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false"
cd $B
(
	export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/bison/bin:$W/venv/bin:/opt/homebrew/bin:$PATH
	export PKG_CONFIG_PATH=$W/host/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/share/pkgconfig
	unset DEVELOPER_DIR; export CC="/usr/bin/clang" CXX="/usr/bin/clang++" OBJC="/usr/bin/clang"
	[ -f build-clc-host/build.ninja ] || meson setup build-clc-host $M --buildtype=release $COMMON \
		-Dvulkan-drivers=panfrost -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled \
		-Dllvm=enabled -Dshared-llvm=enabled > meson-clc-host.log 2>&1 || { tail -30 meson-clc-host.log; exit 1; }
	nice -n 10 ninja -j8 -C build-clc-host src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
		src/panfrost/clc/panfrost_compile > ninja-clc-host.log 2>&1 || { grep -B2 -A12 FAILED ninja-clc-host.log | head -60; exit 1; }
)
echo HOST-TOOLS-OK
export PATH=$B/build-clc-host/src/compiler/clc:$B/build-clc-host/src/compiler/spirv:$B/build-clc-host/src/panfrost/clc:/opt/homebrew/opt/bison/bin:$W/venv/bin:/opt/homebrew/bin:$PATH
[ -f build-vk26/build.ninja ] || meson setup build-vk26 $M --cross-file $W/cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local/starbite --libdir=lib --buildtype=release -Db_ndebug=true $COMMON -Dvulkan-drivers=panfrost \
	-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dshader-cache=disabled -Dlmsensors=disabled > meson-vk26.log 2>&1 || { tail -30 meson-vk26.log; exit 1; }
nice -n 10 ninja -j8 -C build-vk26 > ninja-vk26.log 2>&1 || { grep -B2 -A12 FAILED ninja-vk26.log | head -60; exit 1; }
ls -la build-vk26/src/panfrost/vulkan/libvulkan_panfrost.dylib
echo PANVK-OK
