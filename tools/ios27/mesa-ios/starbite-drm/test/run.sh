#!/bin/sh
# SPDX-License-Identifier: MIT-0
# Builds seq-test.c (starbite_drm.c against a stand-in kernel) on a Mac and runs its three modes.
#   MESA=<an unpacked Mesa 26 tree, for include/drm-uapi> tools/ios27/mesa-ios/starbite-drm/test/run.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA:?MESA= an unpacked Mesa tree (its include/ has drm-uapi/drm.h and panfrost_drm.h)}"
OUT=${TMPDIR:-/tmp}/starbite-seq-test
xcrun clang -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$HERE/.." -I"$MESA/include" \
    -o "$OUT" "$HERE/seq-test.c" -framework CoreFoundation -lpthread
status=0
for mode in off old on; do
	"$OUT" "$mode" || status=1
done
rm -f "$OUT"
rm -rf "$OUT.dSYM"
exit $status
