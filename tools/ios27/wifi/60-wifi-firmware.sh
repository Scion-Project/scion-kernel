#!/bin/bash
# 60-wifi-firmware.sh ROOT [undo] : the BCM4361's Wi-Fi firmware, NVRAM and CLM into ROOT/private/var/hakios/wifi/, where the
# iOS 27 kernel's Wi-Fi (bsd/dev/starlte_wifi.c, boot-args wifi_pcie=6 brcmfmac=N) reads them under bcmdhd's names. Idempotent.
# The files are the phone's own vendor files (Samsung's / Broadcom's), never in any repository: taken from WIFI_FW_DIR
# (your directory, filled from the phone's VENDOR / scion-starlte tools/fetch-firmware.sh; no default) and checked
# against WIFI_FW_SUMS (scion-starlte's firmware/wifi/SHA256SUMS; default: devices/starlte/firmware/wifi/SHA256SUMS of this
# checkout, there after `python3 tools/hakios-device.py sync`) before anything is written. Extras in WIFI_FW_DIR, copied
# only with WIFI_EXTRAS=1 (as docs/ios27/WIFI.md says: a release root never carries a passphrase): mac.info (the phone's
# /efs/wifi/.mac.info) and network (two lines: name, passphrase; PRIVATE: the image then carries it). What was written is listed in /private/var/hakios/wifi/.scion-files; a different file of the same
# name the root had is kept in .scion-orig/ beside it (a file the same as the one given is left alone and not listed).
#   undo  the files of that list taken off the root again, the root's own put back (nothing else is touched).
# Needs root (run by build-full-root.sh under sudo); ROOT is the mounted root volume (owners on).
set -euo pipefail
R=$1
D=$R/private/var/hakios/wifi
[[ -d $R/private/var ]] || { echo "60-wifi-firmware: $R is not a root volume (no private/var)" >&2; exit 1; }
if [[ ${2:-} == undo ]]; then
	n=0
	if [[ -f $D/.scion-files ]]; then
		while IFS= read -r f; do
			[[ -n $f && $f != */* ]] || continue
			rm -f "$D/$f"; n=$((n + 1))
			[[ ! -f $D/.scion-orig/$f ]] || mv -f "$D/.scion-orig/$f" "$D/$f"
		done < "$D/.scion-files"
		rm -f "$D/.scion-files"; rmdir "$D/.scion-orig" 2>/dev/null || true
	fi
	rmdir "$D" 2>/dev/null && { rmdir "$R/private/var/hakios" 2>/dev/null || true; }
	echo "60-wifi-firmware: undone ($n file(s) this step had written taken off, the root's own put back)"
	exit 0
fi
FW=${WIFI_FW_DIR:?60-wifi-firmware: set WIFI_FW_DIR to the directory that holds your phone\'s own Wi-Fi files}
DEF_SUMS=$(cd "$(dirname "$0")/../../.." && pwd)/devices/starlte/firmware/wifi/SHA256SUMS
[[ -n ${WIFI_FW_SUMS:-} || ! -f $DEF_SUMS ]] || WIFI_FW_SUMS=$DEF_SUMS
SUMS=${WIFI_FW_SUMS:?60-wifi-firmware: set WIFI_FW_SUMS to scion-starlte\'s firmware/wifi/SHA256SUMS}
(cd "$FW" && shasum -a 256 -c "$SUMS" >/dev/null) || { echo "60-wifi-firmware: $FW does not match $SUMS" >&2; exit 1; }
mkdir -p "$D"
chown 0:0 "$R/private/var/hakios" "$D" 2>/dev/null || true
chmod 755 "$R/private/var/hakios" "$D"
n=0; touch "$D/.scion-files"
EXTRAS=; [[ ${WIFI_EXTRAS:-0} != 1 ]] || EXTRAS="mac.info network"
for f in $(awk '{print $2}' "$SUMS") $EXTRAS; do
	[[ -f $FW/$f ]] || continue
	if ! grep -qx "$f" "$D/.scion-files"; then
		# the root's own file of that name: left alone when it is the same, else kept for undo (once)
		if [[ -f $D/$f ]]; then
			cmp -s "$FW/$f" "$D/$f" && continue
			mkdir -p "$D/.scion-orig"; cp -p "$D/$f" "$D/.scion-orig/$f"
		fi
		echo "$f" >> "$D/.scion-files"
	fi
	if ! cmp -s "$FW/$f" "$D/$f"; then cp "$FW/$f" "$D/$f"; n=$((n + 1)); fi
	chown 0:0 "$D/$f" 2>/dev/null || true
	case $f in network) chmod 600 "$D/$f" ;; *) chmod 644 "$D/$f" ;; esac
done
chmod 644 "$D/.scion-files"
echo "60-wifi-firmware: $n file(s) written to /private/var/hakios/wifi ($(wc -l < "$D/.scion-files" | tr -d ' ') of this step's there)"
