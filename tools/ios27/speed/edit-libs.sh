#!/bin/bash
# edit-libs.sh LIBDIR ROOT.img : put LIBDIR/libOSMesa.8.dylib and LIBDIR/libsbgl.dylib into the root's /usr/local/starbite/lib, and
# print the first 16 hex digits of the SHA-256 of what was there and of what is there now (the ones before are your way back: copy
# them somewhere first). The optimisation run's root edits are this with the libraries of the run's builds (docs/ios27/OPTIMISE.md):
#   libOSMesa from tools/ios27/mesa-ios/mk-mesa-ios.sh (with the panfrost-opt-*.patch files and the starbite-drm of tools/ios27/mesa-ios),
#   libsbgl from tools/ios27/perf/build-sbgl.sh (eagl.c with the sparse surface hash). Kernel-independent: either kernel boots them.
#   [MNT=<mount point>] tools/ios27/speed/edit-libs.sh LIBDIR ROOT.img
set -eu
D=${1:?usage: edit-libs.sh LIBDIR ROOT.img}; IMG=${2:?usage: edit-libs.sh LIBDIR ROOT.img}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/libsroot.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
L=$M/usr/local/starbite/lib
shasum -a 256 "$L"/*.dylib | cut -c1-16
cp "$D/libOSMesa.8.dylib" "$L/libOSMesa.8.dylib" && cp "$D/libsbgl.dylib" "$L/libsbgl.dylib"
shasum -a 256 "$L"/*.dylib | cut -c1-16
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE
