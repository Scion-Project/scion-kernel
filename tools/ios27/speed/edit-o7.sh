#!/bin/bash
# edit-o7.sh LIBDIR ROOT.img : the optimisation run's first root edit (o7): the libraries of edit-libs.sh (LIBDIR: libOSMesa with
# starbite-drm answering MADVISE in user space, libsbgl with the sparse surface hash), and the one-shot job com.starbite.mtl27test
# (the Metal track's test, tools/ios27/metal) put into launchd's disabled.plist so that it does not run at every boot.
# Undo: edit-libs.sh with the libraries you copied first, and delete com.starbite.mtl27test from
# /private/var/db/com.apple.xpc.launchd/disabled.plist. The later edits of the run (o10: + the MMAP_BO answer cached per handle, o12:
# + render-target renaming on full clears, the final: libsbgl with a full surface hash every 32nd frame) are edit-libs.sh with the
# libraries of those builds (the final pair is what OPTIMISE.md's owner build ran).
#   [MNT=<mount point>] tools/ios27/speed/edit-o7.sh LIBDIR ROOT.img
set -eu
H=$(cd "$(dirname "$0")" && pwd); D=${1:?usage: edit-o7.sh LIBDIR ROOT.img}; IMG=${2:?usage: edit-o7.sh LIBDIR ROOT.img}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/o7root.XXXXXX")}
MNT=$M "$H/edit-libs.sh" "$D" "$IMG" | grep -v EDIT-DONE || exit 1
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
J="$M/private/var/db/com.apple.xpc.launchd/disabled.plist"
/usr/libexec/PlistBuddy -c "Delete :com.starbite.mtl27test" "$J" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :com.starbite.mtl27test bool true" "$J"
plutil -p "$J" | grep -i 'mtl27'
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE
