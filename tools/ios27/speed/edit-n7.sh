#!/bin/bash
# edit-n7.sh IMG [VADDR EXPECT NEW] : the QuartzCore render_for_time clamp patch of the night run (docs/ios27/NIGHT.md).
# CA::WindowServer::Server::render_for_time clamps the next update to now + 0.1 s (10 Hz) unless the display is internal and its
# target power is on, or a power assertion is held; backboardd's own power request had left CA believing the display off. At unslid
# 0x18441a8bc (QuartzCore of the iOS 27.0.1 cache the work used) the clamp is `tbz w20, #0, +0x54` (360002b4); changing it into
# `b +0x54` (14000015) never takes the clamp. cachepatch.py refuses when the word there is another. The subcache is saved to ORIG
# first. Undo: tools/ios27/speed/cachepatch.py CACHEDIR 0x18441a8bc 14000015 360002b4, or put the saved subcache back.
#   [ORIG=<dir>] [MNT=<mount point>] tools/ios27/speed/edit-n7.sh ROOT.img [0x18441a8bc 360002b4 14000015]
set -eu
H=$(cd "$(dirname "$0")" && pwd); IMG=${1:?usage: edit-n7.sh IMG [VADDR EXPECT NEW]}
VA=${2:-0x18441a8bc}; OLD=${3:-360002b4}; NEW=${4:-14000015}
ORIG=${ORIG:-$PWD/orig-n7}; mkdir -p "$ORIG"
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/n7root.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
C=$M/System/Library/Caches/com.apple.dyld
rc=0
SUB=$(python3 -I "$H/cachefind.py" "$C" "$VA") || { echo "$VA is in no subcache"; rc=1; }
if [ $rc = 0 ]; then
	[ -f "$ORIG/$(basename "$SUB")" ] || cp -p "$SUB" "$ORIG/"
	python3 -I "$H/cachepatch.py" "$C" "$VA" "$OLD" "$NEW" || rc=$?
fi
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE rc=$rc
