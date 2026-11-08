#!/usr/bin/env bash
# Build the STARLTE kernel on xnu-12377.121.6 (iOS 26 / macOS 26.5) with the
# ported series in xnu/series-12377: blacktop/darwin-xnu-build at the same
# pinned commit as tools/build-xnu-11417.sh (it knows macOS 26.5 and has
# patches/26.0), KDK 26.5 25F71 (unpacked into build/kdk, not installed).
# Xcode: DEVELOPER_DIR, default Xcode 16.4; Xcode 26 if 16.4's clang or the
# 15.5 SDK is too old for 12377. The 11417 kernel keeps its own tree.
#   tools/build-xnu-12377.sh           -> out/kernel.development.starlte-12377
#   FRESH=1 tools/build-xnu-12377.sh   start from a clean source tree
# darwin-xnu-build runs patches/26.0/*.sh, then applies patches/26.0/*.patch in
# name order and skips the ones that do not apply: our series (0xxx) goes
# first.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
SERIES=$ROOT/xnu/series-12377
DXB=$ROOT/build/dxb-12377
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export DEVELOPER_DIR=${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode (16.4 builds 12377; Xcode 26 if its clang or SDK is too old)}
[ -z "${TOOLS_BIN:-}" ] || export PATH=$TOOLS_BIN:$PATH   # a directory with jq, gum, xcodes, cmake, ninja if they are not on PATH
export MACOS_VERSION=26.5 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=${MACHINE_CONFIG:-STARLTE}
export KDKROOT=${KDKROOT:?set KDKROOT to your unpacked Kernel Debug Kit 26.5 (25F71); Apple, not in this repository}
[ -d "$DEVELOPER_DIR" ] || { echo "Xcode missing at $DEVELOPER_DIR"; exit 1; }
[ -d "$KDKROOT" ] || { echo "KDK missing at $KDKROOT (Kernel_Debug_Kit_26.5_build_25F71.dmg, pkgutil --expand-full; see NOTES.md)"; exit 1; }

for tool in jq gum xcodes cmake ninja; do
    command -v "$tool" >/dev/null || { echo "missing $tool"; exit 1; }
done

series_id=$(cat "$SERIES"/*.patch "$SERIES"/*.sh | shasum -a 256 | cut -c1-16)
if [ ! -d "$DXB" ] || [ -n "${FRESH:-}" ]; then
    rm -rf "$DXB"
    git init -q "$DXB"
    git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
    git -C "$DXB" checkout -q FETCH_HEAD
    # Same firehose flags as the iOS 16 and 11417 builds: plain arm64, no ARMv8.1 LSE atomics.
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
    cp "$SERIES"/*.patch "$SERIES"/*.sh "$DXB"/patches/26.0/
    echo "$series_id" > "$DXB/.series-12377"
else
    # The series is applied once, when the source tree is cloned. make is
    # incremental afterwards; a changed series needs a fresh tree.
    [ "$(cat "$DXB/.series-12377" 2>/dev/null)" = "$series_id" ] || {
        echo "xnu/series-12377 changed since $DXB was set up: run with FRESH=1"; exit 1; }
    [ -d "$DXB/xnu" ] && export SKIP_PATCHES=1
fi
grep -q 'SKIP_PATCHES' "$DXB/build.sh" ||
    sed -i '' 's/^    patches$/    [ -n "${SKIP_PATCHES:-}" ] || patches/' "$DXB/build.sh"

cd "$DXB"
cfg=$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
# build.sh skips the build when this file exists; make itself is incremental.
rm -f "$DXB/build/xnu.obj/kernel.$cfg" "$DXB/fakeroot/System/Library/Kernels/kernel.$cfg"
start=$(date +%s)
touch "$ROOT/build/xnu-12377.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build.sh 2>&1 | tee "$ROOT/build/xnu-12377.log" |
    grep -E --line-buffered "error:|BUILD|Installing|Applying|Skipping|Running script|xnu" | tail -60 || true
kernel=$DXB/fakeroot/System/Library/Kernels/kernel.$cfg
[ -f "$kernel" ] || { echo "no kernel; see build/xnu-12377.log"; exit 1; }
[ "$kernel" -nt "$ROOT/build/xnu-12377.log.start" ] || { echo "kernel was not relinked; see build/xnu-12377.log"; exit 1; }
cp "$kernel" "$ROOT/out/kernel.$cfg-12377"
echo "kernel built in $(( $(date +%s) - start )) s -> out/kernel.$cfg-12377"
