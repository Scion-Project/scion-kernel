#!/bin/bash
# edit-s6.sh IMG [VADDR EXPECT NEW] : the Settings freeze patch: SystemConfiguration's ____wait_for_IOKit_to_quiesce_block_invoke
# starts with pacibsp; making it return at once (RET) ends the wait for configd's InterfaceNamer, which never reports IOKit quiet on
# this kernel (docs/ios27/SPEED.md). The defaults are the address and the instruction words of the iOS 27.0.1 cache the work used
# (unslid 0x1ab5bc514, d503237f -> d65f03c0); cachepatch.py refuses when the word there is not the expected one, so another build
# is told, not patched. Undo: the same with the two words swapped.
#   [MNT=<mount point>] tools/ios27/speed/edit-s6.sh ROOT.img [0x1ab5bc514 d503237f d65f03c0]
set -eu
H=$(cd "$(dirname "$0")" && pwd); IMG=${1:?usage: edit-s6.sh IMG [VADDR EXPECT NEW]}
VA=${2:-0x1ab5bc514}; OLD=${3:-d503237f}; NEW=${4:-d65f03c0}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/s6root.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
python3 "$H/cachepatch.py" "$M/System/Library/Caches/com.apple.dyld" "$VA" "$OLD" "$NEW" || rc=$?
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE rc=${rc:-0}
