#!/usr/bin/env bash
# Build the STARLTE kernel on xnu-12377.121.6 (iOS 26 / macOS 26.5; the base of iOS 27 here) as a macOS-target XNU, from the
# tree xnu/apply-series.sh makes for the `ios-27` profile (patches/xnu-12377/series.ios27): blacktop/darwin-xnu-build at a
# pinned commit does the build around it (libdispatch, bootstrap_cmds, mig, the firehose library), with the Kernel Debug Kit
# 26.5 (25F71) that you bring: Apple's, not in this repository.
#   DEVELOPER_DIR=<Xcode 16.4 or 26> KDKROOT=<unpacked KDK> tools/ios27/build-xnu-12377.sh   -> out/kernel.development.starlte-12377
#   FRESH=1 tools/ios27/build-xnu-12377.sh    start from a clean source tree
# tools/ios27/build-xnu-12377-ios.sh then builds the same tree as an iOS-target XNU.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
DXB=$ROOT/build/dxb-12377
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export DEVELOPER_DIR=${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode (16.4 builds 12377; Xcode 26 if its clang or SDK is too old)}
[ -z "${TOOLS_BIN:-}" ] || export PATH=$TOOLS_BIN:$PATH   # a directory with jq, gum, xcodes, cmake, ninja if they are not on PATH
export MACOS_VERSION=26.5 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=${MACHINE_CONFIG:-STARLTE}
export KDKROOT=${KDKROOT:?set KDKROOT to your unpacked Kernel Debug Kit 26.5 (25F71); Apple, not in this repository}
[ -d "$DEVELOPER_DIR" ] || { echo "Xcode missing at $DEVELOPER_DIR"; exit 1; }
[ -d "$KDKROOT" ] || { echo "KDK missing at $KDKROOT (Kernel_Debug_Kit_26.5_build_25F71.dmg, pkgutil --expand-full)"; exit 1; }
for tool in jq gum xcodes cmake ninja; do
    command -v "$tool" >/dev/null || { echo "missing $tool"; exit 1; }
done

series_id=$(cat patches/xnu-12377/series.ios27 patches/xnu-12377/starlte/*.patch patches/xnu-12377/starlte/*.sh \
    patches/xnu-12377/0120-darwin27-syscalls.patch | shasum -a 256 | cut -c1-16)
if [ ! -d "$DXB" ] || [ -n "${FRESH:-}" ]; then
    rm -rf "$DXB"
    git init -q "$DXB"
    git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
    git -C "$DXB" checkout -q FETCH_HEAD
    # Same firehose flags as the iOS 16 build: plain arm64, no ARMv8.1 LSE atomics.
    python3 - "$DXB/build.sh" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
old = 'ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"'
new = ('ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc '
       '-Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto"')
text = path.read_text()
assert text.count(old) == 1, 'darwin-xnu-build firehose line changed'
text = text.replace(old, new).replace('make install -j8', 'make install -j4')
path.write_text(text)
PY
    # The tree: Apple's tag plus the profile's series (the stage scripts fetch Apple's HFS and libpthread and BearSSL by commit)
    OS_PROFILE=ios-27 "$ROOT/xnu/apply-series.sh" "$DXB/xnu"
    echo "$series_id" > "$DXB/.series-12377"
else
    # The series is applied once, when the source tree is made. make is incremental afterwards; a changed series needs a fresh tree.
    [ "$(cat "$DXB/.series-12377" 2>/dev/null)" = "$series_id" ] || {
        echo "the ios-27 series changed since $DXB was set up: run with FRESH=1"; exit 1; }
fi
export SKIP_PATCHES=1
grep -q 'SKIP_PATCHES' "$DXB/build.sh" ||
    sed -i '' 's/^    patches$/    [ -n "${SKIP_PATCHES:-}" ] || patches/' "$DXB/build.sh"

cd "$DXB"
cfg=$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
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
