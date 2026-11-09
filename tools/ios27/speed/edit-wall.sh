#!/bin/bash
# edit-wall.sh IMG : the PosterBoard data store reset of the night run (docs/ios27/NIGHT.md). The root's PosterBoard store is at a
# schema version (v59) that PosterBoard cannot migrate to the one of iOS 27.0.1 (v61: "Failed to migrate to Version 61 from 59",
# then "Data store could not be created"), so it never starts a poster extension. The store is moved aside, to be made new.
# Undo: move PRBPosterExtensionDataStore.night-bak back to PRBPosterExtensionDataStore.
#   [MNT=<mount point>] tools/ios27/speed/edit-wall.sh ROOT.img
set -eu
IMG=${1:?usage: edit-wall.sh IMG}
M=${MNT:-$(mktemp -d "${TMPDIR:-/tmp}/wallroot.XXXXXX")}
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$M" "$IMG" >/dev/null || { echo "attach failed" >&2; exit 1; }
A="$M/private/var/mobile/Library/Application Support"
if [ -d "$A/PRBPosterExtensionDataStore" ] && [ ! -e "$A/PRBPosterExtensionDataStore.night-bak" ]; then
	mv "$A/PRBPosterExtensionDataStore" "$A/PRBPosterExtensionDataStore.night-bak"
fi
ls -la "$A" | grep -i poster || true
sync; hdiutil detach "$M" >/dev/null 2>&1 || hdiutil detach -force "$M"; echo EDIT-DONE
