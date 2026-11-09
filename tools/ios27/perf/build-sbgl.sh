#!/usr/bin/env bash
# build-sbgl.sh : libsbgl.dylib (tools/ios27/ca-gpu/hakios/eagl.c + gl_osmesa.c, linked against the libOSMesa that mk-mesa-ios.sh built)
# and that libOSMesa, ad hoc signed, into $OUT (default build/sbgl), with their hashes. Needs Mesa 25.0.7 built by mk-mesa-ios.sh
# (build/mesa-ios/) and Xcode 16.4.
#   [DEVELOPER_DIR=<Xcode 16.4>] [OUT=<dir>] tools/ios27/perf/build-sbgl.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
M=$ROOT/build/mesa-ios; MV=$M/mesa-25.0.7; H=$ROOT/tools/ios27/ca-gpu/hakios
OUT=${OUT:-$ROOT/build/sbgl}; G=$OUT/gen; mkdir -p "$G"
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall"
cp "$M/build-pan/src/gallium/targets/osmesa/libOSMesa.8.dylib" "$OUT/libOSMesa.8.dylib"
install_name_tool -id /usr/local/starbite/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
nm -gU "$OUT/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$G/exported-gl.txt"
python3 "$H/mkeagl.py" "$H/eagl_slots.txt" "$MV/src/mapi/glapi/registry/gl.xml" "$G/exported-gl.txt" "$H/eagl.c" > "$G/eagl_gen.h"
$CC -Wno-frame-address -Wno-unused-but-set-variable -dynamiclib -install_name /usr/local/starbite/lib/libsbgl.dylib \
	-I"$G" -I"$H" -I"$MV/include" -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
	"$OUT/libOSMesa.8.dylib" "$H/eagl.c" "$H/gl_osmesa.c" -o "$OUT/libsbgl.dylib"
codesign -f -s - "$OUT/libsbgl.dylib" "$OUT/libOSMesa.8.dylib"
shasum -a 256 "$OUT"/*.dylib
