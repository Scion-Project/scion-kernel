#!/usr/bin/env bash
# Build the STARLTE xnu-12377 kernel as an iOS-target XNU (SDKROOT=iphoneos -> PLATFORM=iPhoneOS, XNU_TARGET_OS_IOS,
# XNU_PLATFORM_iPhoneOS, -miphoneos-version-min). Superseded by `OS_PROFILE=ios-27 make kernel`, which runs the same code
# (tools/ios27/build-xnu-ios.sh, the profile's `builder dxb-ios`) and leaves ./hakios-kernel; this wrapper stays for what calls it by its
# name, and copies the kernel where it always put it:
#   tools/ios27/build-xnu-12377-ios.sh      -> out/kernel.development.starlte-12377-ios
# Objects go to build/dxb-12377/build/xnu-ios.obj. Needs what build-xnu-ios.sh needs (DEVELOPER_DIR, KDKROOT; no step by hand any more:
# the iOS firehose library is made by tools/ios27/firehose-ios.py). XNU_SDKROOT=macosx XNU_EXTRA_MAKEARGS=PLATFORM_FOR_CONFIG=iPhoneOS
# selects the hybrid (macOS toolchain target, iOS config), as it did.
# darwin-xnu-build is blacktop's (MIT, github.com/blacktop/darwin-xnu-build); dxb-ios.patch is a patch to it.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
export OS_PROFILE=ios-27
mkdir -p "$ROOT/build" "$ROOT/out"
log=$ROOT/build/xnu-12377-ios.log
start=$(date +%s)
MAKEFLAGS=${MAKEFLAGS:--k} tools/ios27/build-xnu-ios.sh -j4 2>&1 | tee "$log" |
    grep -E --line-buffered " error:|BUILD|Installing|xnu|^build-xnu-ios|^firehose-ios" | tail -60 || true
kernel=$(tools/ios27/build-xnu-ios.sh --kernel-path)
[ -f "$kernel" ] || { echo "no (re)linked kernel at $kernel; see build/xnu-12377-ios.log"; exit 1; }
cp "$kernel" "$ROOT/out/kernel.$(basename "$kernel" | sed 's/^mach\.//')-12377-ios"
echo "kernel built in $(( $(date +%s) - start )) s from $kernel -> out/kernel.$(basename "$kernel" | sed 's/^mach\.//')-12377-ios"
