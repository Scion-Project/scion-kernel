#!/bin/bash
# edit-s4.sh IMG : the ahead-of-time PACGA -> AND and LDRAA/LDRAB writeback -> LDR rewrite of the root's dyld shared cache
# (a64e-rewrite2.py; libcorecrypto stays untouched). It needs a kernel booted with sweetspeed&4 (sweet_a64e.c: the trapped PACGA
# gives the same value) and the rewritten files are in place: copy the subcaches this changes (it lists them) before, to go back.
#   [PY=python3 with numpy] [MNT=<mount point>] tools/ios27/speed/edit-s4.sh ROOT.img
set -eu
H=$(cd "$(dirname "$0")" && pwd); IMG=${1:?usage: edit-s4.sh IMG}; PY=${PY:-python3}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/s4root.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
C=$M/System/Library/Caches/com.apple.dyld
ls -l -T "$C" > "${TMPDIR:-/tmp}/cache-before-s4.ls"
$PY -I "$H/a64e-rewrite2.py" cache "$C/dyld_shared_cache_arm64e"
ls -l -T "$C" > "${TMPDIR:-/tmp}/cache-after-s4.ls"
diff "${TMPDIR:-/tmp}/cache-before-s4.ls" "${TMPDIR:-/tmp}/cache-after-s4.ls" | grep '^>' | awk '{print $NF}' | tr '\n' ' '; echo
$PY -I "$H/a64e-rewrite2.py" cache "$C/dyld_shared_cache_arm64e" --dry-run | tail -1
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE
