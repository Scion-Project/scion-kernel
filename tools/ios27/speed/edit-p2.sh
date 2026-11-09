#!/bin/bash
# edit-p2.sh IMG [LIBOSMESA] : the root edits of the perf2 run (docs/ios27/NIGHT.md), idempotent, on an iOS 27 root image of the S9
# (a raw image of the root volume, on the Mac):
#   1. kbd: SafariCore's _WBSSetUpAccessToAppDataContainerWithIdentifier starts with pacibsp (unslid 0x1aa75d000, a void function);
#      making it return at once ends the null dereference on its error path when Safari's app data container is missing
#      (cachepatch.py: d503237f -> d65f03c0; it refuses when the word there is another, so another build is told, not patched)
#   2. crash loops: the launchd jobs of disable-p2.list (com.apple.imagent, which starts MessagesBlastDoorService, and
#      com.apple.terminusd) moved into launchd-disabled.plist (edit-s1.py)
#   3. with LIBOSMESA: that file replaces /usr/local/starbite/lib/libOSMesa.8.dylib. Build it with tools/ios27/mesa-ios (its
#      starbite-drm asks the kernel for asynchronous submits; it stays synchronous unless the kernel runs with sweetspeed&64, and
#      STARBITE_DRM_SYNC=1 in the process's environment forces synchronous submits)
# The files the edits change are copied to ORIG (default ./orig-p2) first, to go back. Undo of 1: cachepatch.py with the two words
# swapped, or put the saved subcache back.
#   [ORIG=<dir>] [MNT=<mount point>] tools/ios27/speed/edit-p2.sh ROOT.img [libOSMesa.8.dylib]
set -eu
H=$(cd "$(dirname "$0")" && pwd); IMG=${1:?usage: edit-p2.sh IMG [LIBOSMESA]}; LIB=${2:-}
ORIG=${ORIG:-$PWD/orig-p2}; mkdir -p "$ORIG"
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/p2root.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
C=$M/System/Library/Caches/com.apple.dyld
rc=0
SUB=$(python3 -I "$H/cachefind.py" "$C" 0x1aa75d000) || { echo "0x1aa75d000 is in no subcache"; rc=1; }
if [ $rc = 0 ]; then
	[ -f "$ORIG/$(basename "$SUB")" ] || cp -p "$SUB" "$ORIG/"
	python3 -I "$H/cachepatch.py" "$C" 0x1aa75d000 d503237f d65f03c0 || rc=$?
fi
X=$M/System/Library/xpc
[ -f "$ORIG/launchd.plist" ] || cp -p "$X/launchd.plist" "$X/launchd-disabled.plist" "$ORIG/"
python3 -I "$H/edit-s1.py" "$M" "$H/disable-p2.list" || rc=$?
if [ -n "$LIB" ]; then
	L=$M/usr/local/starbite/lib/libOSMesa.8.dylib
	[ -f "$ORIG/libOSMesa.8.dylib" ] || cp -p "$L" "$ORIG/"
	cat "$LIB" > "$L"
	shasum "$L" "$LIB"
fi
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE rc=$rc
