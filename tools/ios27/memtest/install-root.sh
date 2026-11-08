#!/usr/bin/env bash
# Put build/memtest/launchd.memtest into a raw HFSX root image as
# /usr/appleinternal/sbin/launchd.memtest (root:wheel 0755). Run as root:
#   sudo IMG=build/root-ipados18.img tools/ios27/memtest/install-root.sh
# then write the image with tools/starlte-flash-root.py ... --diff.
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root: sudo $0" >&2; exit 2; }
cd "$(dirname "$0")/../../.."
img=${IMG:-build/root-ipados18.img}
bin=build/memtest/launchd.memtest
[[ -f $img && -f $bin ]] || { echo "missing $img or $bin" >&2; exit 1; }
dev=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$img" | awk 'NR==1 {print $1}')
mnt=$(mktemp -d /tmp/iosroot.XXXX)
trap 'umount "$mnt" 2>/dev/null || true; hdiutil detach "$dev" >/dev/null 2>&1 || true; rmdir "$mnt"' EXIT
mount -t hfs -o owners "$dev" "$mnt"
install -d -o 0 -g 0 -m 0755 "$mnt/usr/appleinternal/sbin"
install -o 0 -g 0 -m 0755 "$bin" "$mnt/usr/appleinternal/sbin/launchd.memtest"
ls -l "$mnt/usr/appleinternal/sbin/"
sync
echo "installed launchd.memtest into $img"
