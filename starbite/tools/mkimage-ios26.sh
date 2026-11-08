#!/usr/bin/env bash
# iOS 26 boot image: the 12377 kernel (strip -x copy, to fit BOOT) and the
# iPhone 11 ADT (256-byte random-seed; mkimage-11417.sh's default ADT has 64
# and the kernel panics in prng_random).
#   [ADT=out/starlte-iphone11-1792.adt] tools/mkimage-ios26.sh NAME CMDLINE_FILE   -> out/xnu-boot-NAME.img
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
cp out/kernel.development.starlte-12377 out/kernel.development.starlte-12377.x
xcrun strip -x out/kernel.development.starlte-12377.x
ADT=${ADT:-out/starlte-iphone11.adt} KERNEL=out/kernel.development.starlte-12377.x exec tools/mkimage-11417.sh "$@"
