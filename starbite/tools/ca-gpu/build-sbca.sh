#!/usr/bin/env bash
# Build the backboardd GPU libraries (sbca.c: iOS 26 by default, -DSBCA_IOS26=0 for iPadOS 18), ad-hoc signed:
#   out/ios/libsbca.dylib  tools/ca-gpu/sbca.c, the renderer hook (on the root as /usr/lib/sbca.dylib)
#   out/ios/libsbgl.dylib  HakiOS's EAGLContext over Mesa, tools/ca-gpu/hakios (on the root as
#                          /usr/local/starbite/lib/libsbgl.dylib, beside libOSMesa.8.dylib)
# Needs out/ios/libOSMesa.8.dylib (tools/mesa-ios) and Mesa's gl.xml from its build tree.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall"
H=tools/ca-gpu/hakios
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
	tools/ca-gpu/sbca.c -o out/ios/libsbca.dylib
codesign -f -s - out/ios/libsbgl.dylib out/ios/libsbca.dylib
ls -la out/ios/libsbgl.dylib out/ios/libsbca.dylib
