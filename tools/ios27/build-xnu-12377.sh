#!/usr/bin/env bash
# Build the STARLTE kernel on xnu-12377.121.6 (iOS 26 / macOS 26.5; the base of iOS 27 here) as a macOS-target XNU, from the tree
# xnu/apply-series.sh makes for the `ios-27` profile: blacktop/darwin-xnu-build at the commit of tools/ios27/dxb.pin does the build
# around it (libdispatch, bootstrap_cmds, mig, the firehose library), with the Kernel Debug Kit 26.5 (25F71) that you bring: Apple's,
# not in this repository. The setup of build/dxb-12377 is tools/ios27/dxb-common.sh's, shared with the iOS-target build.
#   DEVELOPER_DIR=<Xcode 16.4 or 26> KDKROOT=<unpacked KDK> tools/ios27/build-xnu-12377.sh   -> out/kernel.development.starlte-12377
#   FRESH=1 tools/ios27/build-xnu-12377.sh    start from a clean source tree
# The kernel of the profile is the iOS-target one, which `OS_PROFILE=ios-27 make kernel` builds (tools/ios27/build-xnu-ios.sh;
# docs/ios27/IOS27-TARGET.md); this macOS-target build of the same tree is kept for comparison and for what was measured on it.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
DXB_WHO=build-xnu-12377
. "$ROOT/tools/ios27/dxb-common.sh"
export OS_PROFILE=ios-27
dxb_env
dxb_setup

cd "$DXB"
cfg=$(dxb_cfg)
# build.sh skips the build when this file exists; make itself is incremental.
rm -f "$DXB/build/xnu.obj/kernel.$cfg" "$DXB/fakeroot/System/Library/Kernels/kernel.$cfg"
start=$(date +%s)
mkdir -p "$ROOT/build" "$ROOT/out"
touch "$ROOT/build/xnu-12377.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build.sh 2>&1 | tee "$ROOT/build/xnu-12377.log" |
    grep -E --line-buffered "error:|BUILD|Installing|Applying|Skipping|Running script|xnu" | tail -60 || true
kernel=$DXB/fakeroot/System/Library/Kernels/kernel.$cfg
[ -f "$kernel" ] && [ "$kernel" -nt "$ROOT/build/xnu-12377.log.start" ] || { echo "no new kernel at $kernel; see build/xnu-12377.log"; exit 1; }
cp "$kernel" "$ROOT/out/kernel.$cfg-12377"
echo "kernel built in $(( $(date +%s) - start )) s -> out/kernel.$cfg-12377"
