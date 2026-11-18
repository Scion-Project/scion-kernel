#!/bin/bash
# SPDX-License-Identifier: MIT-0
# host-bench: the native Metal bridge's CPU cost on the Mac, with Mesa's KosmicKrisp (Vulkan over Metal) as its driver.
#   W=<the iOS 27 Metal work dir with build/mesa/mesa-26.2.6 (mk-panvk27.sh's)> tools/ios27/metal/host-bench/build.sh
# Needs: an Xcode whose macOS SDK has Metal 4 (KosmicKrisp includes MTL4*.h; Xcode 16.4's SDK does not: XCODE=/Applications/Xcode.app),
# Homebrew llvm (the kk_clc host tool), the SPIRV host libraries of build-host.sh ($W/host or HOST_PREFIX=), mk-panvk27.sh's venv,
# and the bridge built for the host (tools/ios27/metal2/build-m2v-bridge.sh host, with bridge.sh's Rust environment).
# Output: $W/host-bench/{bench,libkkshim.dylib}. Run: $W/host-bench/bench $W/host-bench/libkkshim.dylib /tmp/m2v-cache 400 100 16 1
set -euo pipefail
SB=$(cd "$(dirname "$0")/../../../.." && pwd); W=${W:?set W}; B=$W/build/mesa; M=$B/mesa-26.2.6; O=$W/host-bench
XCODE=${XCODE:-/Applications/Xcode.app}; HOST_PREFIX=${HOST_PREFIX:-$W/host}
mkdir -p $O; cd $B
# 1. kk_clc, mesa_clc and vtn_bindgen2 for the host (LLVM)
(
	export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/bison/bin:$W/venv/bin:/opt/homebrew/bin:$PATH
	export PKG_CONFIG_PATH=$HOST_PREFIX/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/share/pkgconfig
	export DEVELOPER_DIR=$XCODE/Contents/Developer SDKROOT=$(DEVELOPER_DIR=$XCODE/Contents/Developer xcrun --sdk macosx --show-sdk-path)
	export CC=/usr/bin/clang CXX=/usr/bin/clang++ OBJC=/usr/bin/clang OBJCXX=/usr/bin/clang++
	[ -f build-kkhost/build.ninja ] || meson setup build-kkhost $M --buildtype=release -Dplatforms= -Dglx=disabled -Degl=disabled \
		-Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dgallium-drivers= -Dvulkan-drivers=kosmickrisp -Dmesa-clc=enabled \
		-Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dllvm=enabled -Dshared-llvm=enabled -Dzstd=disabled -Dexpat=disabled > meson-kkhost.log
	nice -n 10 ninja -j4 -C build-kkhost src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/kosmickrisp/clc/kk_clc > ninja-kkhost.log
)
# 2. the KosmicKrisp ICD
HT=$B/build-kkhost
(
	export DEVELOPER_DIR=$XCODE/Contents/Developer
	export PATH=$HT/src/kosmickrisp/clc:$HT/src/compiler/clc:$HT/src/compiler/spirv:/opt/homebrew/opt/bison/bin:$W/venv/bin:/opt/homebrew/bin:$PATH
	export SDKROOT=$(xcrun --sdk macosx --show-sdk-path) TC=$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin
	export CC=$TC/clang CXX=$TC/clang++ OBJC=$TC/clang OBJCXX=$TC/clang++
	[ -f build-kk/build.ninja ] || meson setup build-kk $M --buildtype=release -Db_ndebug=true -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
		-Dplatforms= -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dmesa-clc=system \
		-Dprecomp-compiler=system -Dshader-cache=disabled -Dlmsensors=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled > meson-kk.log
	nice -n 10 ninja -j4 -C build-kk > ninja-kk.log
)
K=$B/build-kk/src/kosmickrisp/vulkan
# 3. the shim that lets metal2vulkan open the ICD as a loader (it looks up vkGetInstanceProcAddr), and the bench
export DEVELOPER_DIR=$XCODE/Contents/Developer
xcrun clang -dynamiclib -o $O/libkkshim.dylib $SB/tools/ios27/metal/host-bench/kkshim.c -L$K -lvulkan_kosmickrisp -Wl,-rpath,$K
xcrun clang -O2 -g -o $O/bench $SB/tools/ios27/metal/host-bench/bench.c -I $SB/tools/ios27/metal2/bridge \
	$SB/build/m2v-bridge/host/libnative_metal_bridge.a -lc++ -framework Foundation -framework Metal -framework IOSurface \
	-framework CoreFoundation -framework Security
echo "HOST-BENCH-OK $O"
