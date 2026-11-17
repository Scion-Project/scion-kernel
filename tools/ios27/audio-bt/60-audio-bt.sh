#!/bin/bash
# 60-audio-bt.sh ROOT [undo] : the Galaxy S9's audio and Bluetooth on an iOS 27 root (Scion, private). By hand it is OFF unless
# AUDIO_BT=1; the root build's step fullroot/steps.d/92-audio-bt.sh runs it by default. Idempotent. Needs the kernel's abox=8
# (+ max98512, abox_rmem=1 or extra_ram) for sound and bt=7 for Bluetooth (all on by default since 17 November 2026); without
# them the plug-in finds no /dev/hakios-audio and bluetoothd no UART.
#   AUDIO_BT=1            do it
#   OS_MNT=<dir>          the IPSW's OS image mounted (System/Library/Audio and the jobs, if the root lacks them)
#   HCD=<file>            the phone's own Broadcom patch, bcm4361B2_semco.hcd (or _murata) from its VENDOR partition
#                         (both test phones: semco); installed as /private/var/hakios/bt/<its name> for bt=6; checked against
#                         HCD_SUMS (default devices/starlte/firmware/bt/SHA256SUMS, when this checkout has it) by name and sha256
#   FILES=<dir>           what build.sh made (default build/ios27-audio-bt/files of this checkout)
#   undo                  everything this step put on the root taken off, the three launchd jobs back where they were
#                         (edit-launchd-audio-bt.py keeps that in /usr/local/share/scion/audio-bt-jobs.plist), BlueTool's
#                         scripts the root had before put back (kept as NAME.scion-orig)
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd); SK=$(cd "$H/../../.." && pwd); F=${FILES:-$SK/build/ios27-audio-bt/files}
[[ -d $R/System/Library ]] || { echo "60-audio-bt: $R is not an iOS root" >&2; exit 1; }
A=$R/System/Library/Audio; E=$R/private/etc/bluetool; B=$R/private/var/hakios/bt
if [[ ${2:-} == undo ]]; then
	/usr/bin/python3 "$H/edit-launchd-audio-bt.py" "$R" --undo
	rm -rf "$A/Plug-Ins/HAL/HakiosAudio.driver"
	rm -f "$R/usr/libexec/hakios-inject" "$R/usr/lib/hakios.bluetool.dylib" "$R/usr/lib/hakios.bluetoothd.dylib"
	for s in "$H"/bluetool/*.script; do
		n=$(basename "$s")
		if [[ -f $E/$n.scion-orig ]]; then mv -f "$E/$n.scion-orig" "$E/$n"; else rm -f "$E/$n"; fi
	done
	rm -f "$E/.scion-new"; rmdir "$E" 2>/dev/null || true
	if [[ -f $B/.scion-files ]]; then
		while IFS= read -r f; do
			[[ -n $f && $f != */* ]] || continue
			rm -f "$B/$f"; [[ ! -f $B/.scion-orig/$f ]] || mv -f "$B/.scion-orig/$f" "$B/$f"
		done < "$B/.scion-files"
		rm -f "$B/.scion-files"; rmdir "$B/.scion-orig" 2>/dev/null || true
	fi
	rmdir "$B" 2>/dev/null && { rmdir "$R/private/var/hakios" 2>/dev/null || true; }
	[[ ! -f $A/.scion-from-ipsw ]] || rm -rf "$A"
	echo "60-audio-bt: undone"
	exit 0
fi
[[ ${AUDIO_BT:-0} == 1 ]] || { echo "60-audio-bt: AUDIO_BT not set: nothing done"; exit 0; }
for f in HakiosAudio.driver/HakiosAudio hakios-inject hakios.bluetool.dylib hakios.bluetoothd.dylib; do
	[[ -f $F/$f ]] || { echo "60-audio-bt: no $F/$f (tools/ios27/audio-bt/build.sh)" >&2; exit 1; }
done
if [[ -n ${HCD:-} ]]; then
	SUMS=${HCD_SUMS:-$SK/devices/starlte/firmware/bt/SHA256SUMS}
	if [[ -f $SUMS ]]; then
		grep -q "^$(shasum -a 256 "$HCD" | cut -c1-64)  $(basename "$HCD")\$" "$SUMS" ||
			{ echo "60-audio-bt: $HCD is not one of the patches $SUMS names (by name and sha256)" >&2; exit 1; }
	fi
fi
if [[ ! -d $A/Plug-Ins/HAL/BTAudioHALPlugin.driver ]]; then
	[[ -n ${OS_MNT:-} && -d $OS_MNT/System/Library/Audio ]] || { echo "60-audio-bt: $A missing and no OS_MNT" >&2; exit 1; }
	[[ -d $A ]] || { mkdir -p "$A"; : > "$A/.scion-from-ipsw"; }
	/usr/bin/rsync -aH "$OS_MNT/System/Library/Audio/" "$A/"
	echo "60-audio-bt: System/Library/Audio from the IPSW ($(du -sh "$A" | cut -f1))"
fi
rm -rf "$A/Plug-Ins/HAL/HakiosAudio.driver"; cp -R "$F/HakiosAudio.driver" "$A/Plug-Ins/HAL/"
install -m 755 "$F/hakios-inject" "$R/usr/libexec/hakios-inject"
install -m 755 "$F/hakios.bluetool.dylib" "$F/hakios.bluetoothd.dylib" "$R/usr/lib/"
mkdir -p "$E"
for s in "$F"/bluetool/*.script; do
	n=$(basename "$s")
	# the root's own script of that name kept once (BlueTool's .scion-new lists the names this step added)
	if [[ ! -f $E/$n ]]; then
		echo "$n" >> "$E/.scion-new"
	elif [[ ! -f $E/$n.scion-orig ]] && ! grep -qx "$n" "$E/.scion-new" 2>/dev/null; then
		cp -p "$E/$n" "$E/$n.scion-orig"
	fi
	install -m 644 "$s" "$E/$n"
done
if [[ -n ${HCD:-} ]]; then
	h=$(basename "$HCD"); mkdir -p "$B"; touch "$B/.scion-files"
	if ! grep -qx "$h" "$B/.scion-files"; then
		# the root's own patch of that name: left alone when it is the same, else kept for undo
		if [[ -f $B/$h ]] && ! cmp -s "$HCD" "$B/$h"; then mkdir -p "$B/.scion-orig"; cp -p "$B/$h" "$B/.scion-orig/$h"; fi
		[[ -f $B/$h ]] || echo "$h" >> "$B/.scion-files"
		[[ ! -f $B/.scion-orig/$h ]] || echo "$h" >> "$B/.scion-files"
	fi
	cmp -s "$HCD" "$B/$h" || install -m 644 "$HCD" "$B/$h"
else
	echo "60-audio-bt: no HCD given: bt=6 will find no patch (put the phone's own .hcd in /private/var/hakios/bt/)"
fi
[[ ${AUDIO_BT_LOG:-0} == 1 ]] && { mkdir -p "$R/private/var/local/oslog"; chmod 777 "$R/private/var/local/oslog"; }
/usr/bin/python3 "$H/edit-launchd-audio-bt.py" "$R" ${OS_MNT:+"$OS_MNT/System/Library/xpc/launchd.plist"}
echo "60-audio-bt: done"
