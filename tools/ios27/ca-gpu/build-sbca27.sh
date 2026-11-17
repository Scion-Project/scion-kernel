#!/usr/bin/env bash
# Build sbca, libsbgl (HakiOS's EAGL bridge over Mesa's OSMesa) and the OSMesa dylib for the iOS 27 root. sbca gets the
# QuartzCore table of the iOS build (tools/ios27/builds/DEFAULT, or IOS27_BUILD=<build> with a <build>-sbca.txt).
# Output in out/ (next to this directory's parent): libsbca.dylib (and libsbca.BUILD), libsbgl.dylib, libOSMesa.8.dylib.
#   SB=<Starbite tree> MESA=<mesa-25.0.7 source tree> OSMESA=<built libOSMesa.8.dylib> tools/ios27/ca-gpu/build-sbca27.sh
# Mesa and its Panfrost build come from tools/ios27/mesa-ios (mk-mesa-ios.sh); nothing of Apple's is needed but the iPhoneOS SDK.
set -euo pipefail
cd "$(dirname "$0")/../../.."
# the QuartzCore table of the iOS build (userland/src/sbca-qc/<BUILD>.h, from tools/ios27/builds/<BUILD>-sbca.txt)
. tools/ios27/builds/build-conf.sh
BUILD=${IOS27_BUILD:-$(ios27_build_default)}
[ -f "userland/src/sbca-qc/$BUILD.h" ] || { echo "no sbca table for $BUILD: tools/ios27/builds/$BUILD-sbca.txt, then tools/ios27/ca-gpu/sbca_table.py sync" >&2; exit 1; }
SB=${SB:-$PWD}
MESA=${MESA:?set MESA to the mesa-25.0.7 source tree}
OSMESA=${OSMESA:?set OSMESA to the built libOSMesa.8.dylib (tools/ios27/mesa-ios/mk-mesa-ios.sh)}
: "${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode}"
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall"
H=tools/ios27/ca-gpu/hakios; G=build/sbca27; mkdir -p out $G
cp "$OSMESA" out/libOSMesa.8.dylib
install_name_tool -id /usr/local/starbite/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > $G/exported-gl.txt
python3 $H/mkeagl.py $H/eagl_slots.txt "$MESA/src/mapi/glapi/registry/gl.xml" $G/exported-gl.txt $H/eagl.c > $G/eagl_gen.h
$CC -Wno-frame-address -Wno-unused-but-set-variable -dynamiclib -install_name /usr/local/starbite/lib/libsbgl.dylib \
	-I$G -I$H -I"$MESA/include" -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
	out/libOSMesa.8.dylib $H/eagl.c $H/gl_osmesa.c -o out/libsbgl.dylib
$CC -Wno-unused-function -dynamiclib -install_name /usr/lib/sbca.dylib -lobjc -framework IOSurface -framework CoreFoundation \
	-DSBCA_QC_TABLE="\"sbca-qc/$BUILD.h\"" userland/src/sbca.c -o out/libsbca.dylib
echo "$BUILD" > out/libsbca.BUILD   # the iOS build this sbca is for
codesign -f -s - out/libsbgl.dylib out/libsbca.dylib out/libOSMesa.8.dylib
ls -la out/
