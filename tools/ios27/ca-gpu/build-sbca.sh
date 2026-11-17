#!/usr/bin/env bash
# Build the backboardd GPU libraries (sbca.c: iOS 26 by default, -DSBCA_IOS26=0 for iPadOS 18), ad-hoc signed:
#   out/ios/libsbca.dylib  userland/src/sbca.c, the renderer hook (on the root as /usr/lib/sbca.dylib)
#   out/ios/libsbgl.dylib  HakiOS's EAGLContext over Mesa, tools/ios27/ca-gpu/hakios (on the root as
#                          /usr/local/starbite/lib/libsbgl.dylib, beside libOSMesa.8.dylib)
# Needs out/ios/libOSMesa.8.dylib (tools/ios27/mesa-ios) and Mesa's gl.xml from its build tree.
set -euo pipefail
cd "$(dirname "$0")/../../.."
# the QuartzCore table of the iOS build (userland/src/sbca-qc/<BUILD>.h, from tools/ios27/builds/<BUILD>-sbca.txt)
. tools/ios27/builds/build-conf.sh
BUILD=${IOS27_BUILD:-$(ios27_build_default)}
[ -f "userland/src/sbca-qc/$BUILD.h" ] || { echo "no sbca table for $BUILD: tools/ios27/builds/$BUILD-sbca.txt, then tools/ios27/ca-gpu/sbca_table.py sync" >&2; exit 1; }
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall"
H=tools/ios27/ca-gpu/hakios
G=build/ca-gpu
mkdir -p out/ios $G

# the GL dispatch table iOS's OpenGLES has, filled from what our Mesa exports
nm -gU out/ios/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > $G/exported-gl.txt
python3 $H/mkeagl.py $H/eagl_slots.txt build/mesa-ios/mesa-25.0.7/src/mapi/glapi/registry/gl.xml \
	$G/exported-gl.txt $H/eagl.c > $G/eagl_gen.h

$CC -Wno-frame-address -Wno-unused-but-set-variable -dynamiclib \
	-install_name /usr/local/starbite/lib/libsbgl.dylib -I$G -I$H -Ibuild/mesa-ios/mesa-25.0.7/include \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
	out/ios/libOSMesa.8.dylib $H/eagl.c $H/gl_osmesa.c -o out/ios/libsbgl.dylib
$CC -Wno-unused-function -dynamiclib -install_name /usr/lib/sbca.dylib -lobjc -framework IOSurface -framework CoreFoundation \
	-DSBCA_QC_TABLE="\"sbca-qc/$BUILD.h\"" userland/src/sbca.c -o out/ios/libsbca.dylib
echo "$BUILD" > out/ios/libsbca.BUILD   # the iOS build this sbca is for
codesign -f -s - out/ios/libsbgl.dylib out/ios/libsbca.dylib
ls -la out/ios/libsbgl.dylib out/ios/libsbca.dylib
