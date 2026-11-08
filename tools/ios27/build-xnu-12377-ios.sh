#!/usr/bin/env bash
# Build the STARLTE xnu-12377 kernel as an iOS-target XNU (SDKROOT=iphoneos -> PLATFORM=iPhoneOS, XNU_TARGET_OS_IOS,
# XNU_PLATFORM_iPhoneOS, -miphoneos-version-min), from the tree tools/ios27/build-xnu-12377.sh already set up and patched in
# build/dxb-12377 (which builds it as a macOS-target XNU). Objects go to build/dxb-12377/build/xnu-ios.obj.
#   tools/ios27/build-xnu-12377-ios.sh      -> out/kernel.development.starlte-12377-ios
# XNU_SDKROOT=macosx XNU_EXTRA_MAKEARGS=PLATFORM_FOR_CONFIG=iPhoneOS selects the hybrid (macOS toolchain target, iOS config).
# What it needs beyond the macOS-target build:
#   - tools/ios27/dxb-ios.patch applied to darwin-xnu-build's build.sh (this script does it: build-ios.sh);
#   - build/dxb-12377/build/firehose-ios/libfirehose_kernel.a: the macOS-built firehose_buffer.o with its LC_BUILD_VERSION
#     changed to iOS 18.5 (ld refuses a macOS object in an iOS link). Not scripted yet; vtool -set-build-version
#     refuses the object ("not enough space to hold load commands"), so it was patched by hand.
# darwin-xnu-build is blacktop's (MIT, github.com/blacktop/darwin-xnu-build); dxb-ios.patch is a patch to it.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
DXB=$ROOT/build/dxb-12377
export DEVELOPER_DIR=${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode (16.4 builds 12377; Xcode 26 if its clang or SDK is too old)}
[ -z "${TOOLS_BIN:-}" ] || export PATH=$TOOLS_BIN:$PATH
export MACOS_VERSION=26.5 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=${MACHINE_CONFIG:-STARLTE}
export KDKROOT=${KDKROOT:?set KDKROOT to your unpacked Kernel Debug Kit 26.5 (25F71); Apple, not in this repository}
export SKIP_PATCHES=1
[ -d "$DXB/xnu" ] || { echo "run tools/ios27/build-xnu-12377.sh once first: it sets up $DXB with the series applied"; exit 1; }
[ -f "$DXB/build/firehose-ios/libfirehose_kernel.a" ] || { echo "missing $DXB/build/firehose-ios/libfirehose_kernel.a (see this script's head)"; exit 1; }
cd "$DXB"
cp build.sh build-ios.sh
patch -s -p1 build-ios.sh < "$ROOT/tools/dxb-ios.patch" 2>/dev/null || patch -s build-ios.sh < "$ROOT/tools/dxb-ios.patch"
cfg=$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
start=$(date +%s)
touch "$ROOT/build/xnu-12377-ios.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build-ios.sh 2>&1 | tee "$ROOT/build/xnu-12377-ios.log" |
    grep -E --line-buffered " error:|BUILD|Installing|xnu" | tail -60 || true
# the iOS-target kernel file is mach.<config>.<machine>, not kernel.*
kernel=$(find -L "$DXB/build/xnu-ios.obj" -maxdepth 1 -type f \( -name "kernel.$cfg" -o -name "mach.$cfg" \) -newer "$ROOT/build/xnu-12377-ios.log.start" | head -1)
[ -n "$kernel" ] || { echo "no (re)linked kernel in build/xnu-ios.obj; see build/xnu-12377-ios.log"; exit 1; }
mkdir -p "$ROOT/out"
cp "$kernel" "$ROOT/out/kernel.$cfg-12377-ios"
echo "kernel built in $(( $(date +%s) - start )) s from $kernel -> out/kernel.$cfg-12377-ios"
