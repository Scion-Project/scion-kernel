#!/bin/bash
# Host tools for Mesa 26's precompiled kernels (mesa_clc needs them): SPIRV-Headers, SPIRV-Tools and SPIRV-LLVM-Translator v23.1.3 built
# against Homebrew llvm 23 -> $W/host (mk-panvk27.sh puts $W/host/lib/pkgconfig on PKG_CONFIG_PATH).
#   [W=<work dir, default build/metal27>] tools/ios27/metal/build-host.sh
set -euo pipefail
W=${W:-$PWD/build/metal27}; P=$W/host; mkdir -p $W/src $P; cd $W/src
export PATH=/opt/homebrew/bin:$PATH
[ -d SPIRV-Headers ] || git clone -q --depth 1 -b vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Headers
[ -d SPIRV-Tools ] || git clone -q --depth 1 -b vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Tools
[ -d SPIRV-LLVM-Translator ] || git clone -q --depth 1 -b v23.1.3 https://github.com/KhronosGroup/SPIRV-LLVM-Translator
cmake -S SPIRV-Headers -B b-hdr -G Ninja -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C b-hdr install >/dev/null
cmake -S SPIRV-Tools -B b-tools -G Ninja -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DSPIRV-Headers_SOURCE_DIR=$W/src/SPIRV-Headers \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
nice -n 10 ninja -j4 -C b-tools install >/dev/null
cmake -S SPIRV-LLVM-Translator -B b-tr -G Ninja -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=$W/src/SPIRV-Headers \
  -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
nice -n 10 ninja -j4 -C b-tr install >/dev/null
ls $P/lib/pkgconfig
echo HOST-DONE
