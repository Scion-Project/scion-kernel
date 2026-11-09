#!/bin/bash
# edit-s1.sh IMG [LIST] : attach an iOS 27 root image of the S9 (a raw HFS+/APFS image of the root volume, on the Mac), move the launchd jobs
# of LIST (default trim-s1.list: 86 daemons the S9 gains nothing from) into launchd-disabled.plist, detach.
#   MNT=<mount point> tools/ios27/speed/edit-s1.sh ROOT.img
set -eu
H=$(cd "$(dirname "$0")" && pwd); IMG=${1:?usage: edit-s1.sh IMG [LIST]}; LIST=${2:-$H/trim-s1.list}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/s1root.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
python3 "$H/edit-s1.py" "$M" "$LIST"
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE
