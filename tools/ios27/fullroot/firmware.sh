#!/bin/bash
# SPDX-License-Identifier: MIT-0
# firmware.sh : the Galaxy S9's vendor firmware for the iOS 27 root and boot image, in one place. Sourced by feature.sh (so by
# every feature step), by build-full-root.sh and by tools/ios27/s9/make-boot-image.sh; run by itself:
#   firmware.sh prepare        make it complete and checked (fetching what is missing), then print its directory
#   firmware.sh check          only check what is in place against the sums (exit 1 if anything is missing or wrong)
#   firmware.sh summary ROOT   one line per kind: on the mounted root and checked, or absent and why (exit 1 if a kind a
#                              feature that is on needs is absent)
#
# The files are Samsung's and Broadcom's and are in no repository of ours; only their sums are, in the device repository's
# firmware/<kind>/SHA256SUMS (devices/starlte, scion-starlte, cloned by `python3 tools/hakios-device.py sync`). Its
# tools/fetch-firmware.sh fetches them into firmware/<kind>/ beside the sums (git-ignored there, and devices/ here), from
# FIRMWARE_FROM=DIR (your own copy) or else the public vendor trees it pins at fixed commits, checks each one against its
# sum and keeps what is right for the next build. Under sudo it runs as the builder's own user. The root image the build
# makes carries the files, so keep it private, as before; nothing here puts a file into git, a bundle or a release.
#
# Where each kind goes, and who reads it (the iOS 27 kernel reads nothing of the phone's VENDOR partition: it has no reader
# of it, xnu bsd/dev/starlte_wifi.c):
#   wifi       /private/var/hakios/wifi on the root (steps.d/91-wifi.sh): brcmfmac's firmware, NVRAMs and CLM under bcmdhd's
#              names; the kernel takes the NVRAM of the module the chip's OTP names
#   bt         /private/var/hakios/bt on the root (steps.d/92-audio-bt.sh): both modules' patches; the kernel takes the
#              module's patch the Wi-Fi read from the chip's OTP (bt_otp_wait), or with only one there that one
#   sensorhub  the boot image's device tree, /scion-firmware (make-boot-image.sh, tools/ios27/adt-firmware.py): the hub's
#              image is loaded before there is a root
#   audio, media and any other kind: checked, not put anywhere: no driver of the iOS 27 kernel loads them (ABOX's own CPU is
#              not started, the speakers' DSM does not run; the video decoder is the 16.5 kernel's only)
# Not from here, and never written by anything of this: the phone's per-device files (EFS: the Wi-Fi's MAC address, the
# Bluetooth address, the modem's NV data), which the kernel reads from the phone itself, read only, or does without.
#
# Environment:
#   FIRMWARE_DIR=DIR     a directory laid out as firmware/ (<kind>/SHA256SUMS and the files): checked, never fetched into.
#                        Default devices/$FIRMWARE_DEVICE/firmware of this checkout, completed by its fetch-firmware.sh
#   FIRMWARE_DEVICE      the device repository (starlte)
#   FIRMWARE_FROM=DIR    fetch-firmware.sh --from DIR (your phone's /vendor/firmware, an unpacked VENDOR, a vendor tree)
#   FIRMWARE_OFFLINE=1   fetch-firmware.sh --offline: only FIRMWARE_FROM and what is already in place
# A file that is missing or does not match its sum stops whoever asked (the root build, the boot image), saying what to do:
# no root is built without a feature's firmware while the feature is on. ENABLE_WIFI=0, ENABLE_AUDIO_BT=0 (the root) and
# make-boot-image.sh --firmware none leave a feature out on purpose.
# Not run on a phone: the files are the ones the phone has run (WIFI.md, SENSORS.md, audio-bt/README.md); this way of putting
# them in place has been run against scratch roots only (tools/ios27/fullroot/firmware-test.sh).

FW_SK=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
FW_ROOT_DIR_wifi=private/var/hakios/wifi
FW_ROOT_DIR_bt=private/var/hakios/bt

fw_say() { echo "firmware: $*" >&2; }

if command -v shasum >/dev/null 2>&1; then fw_sha() { shasum -a 256 "$1" | cut -c1-64; }
else fw_sha() { sha256sum "$1" | cut -c1-64; }; fi

# fw_as_builder CMD... : CMD as the builder's own user when this runs under sudo (no root-owned files in the checkout), with
# DEVELOPER_DIR and PY passed through; as the caller otherwise
fw_as_builder() {
	if [[ $EUID == 0 && -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
		sudo -u "$SUDO_USER" -H env PATH="$PATH" ${DEVELOPER_DIR:+DEVELOPER_DIR="$DEVELOPER_DIR"} ${PY:+PY="$PY"} "$@"
	else
		"$@"
	fi
}

# fw_check_kind DIR SUMS : every file SUMS names is in DIR with its sum; the others are listed (stderr), 1 if any
fw_check_kind() {
	local dir=$1 sums=$2 want name bad=0
	[[ -f $sums ]] || { fw_say "no $sums"; return 1; }
	while read -r want name; do
		case $want in '' | '#'*) continue ;; esac
		name=${name#\*}
		if [[ ! -f $dir/$name ]]; then
			fw_say "missing: $dir/$name"; bad=1
		elif [[ $(fw_sha "$dir/$name") != "$want" ]]; then
			fw_say "WRONG SUM: $dir/$name (not the pinned file)"; bad=1
		fi
	done < "$sums"
	return $bad
}

# fw_check DIR : every kind of DIR (each <kind>/SHA256SUMS)
fw_check() {
	local sf bad=0 n=0
	for sf in "$1"/*/SHA256SUMS; do
		[[ -f $sf ]] || continue
		n=$((n + 1))
		fw_check_kind "$(dirname "$sf")" "$sf" || bad=1
	done
	[[ $n -gt 0 ]] || { fw_say "no <kind>/SHA256SUMS under $1"; return 1; }
	return $bad
}

# fw_prepare : FIRMWARE_DIR made complete and checked, exported (once a build: FIRMWARE_READY); 1, having said why, if not
fw_prepare() {
	[[ -z ${FIRMWARE_READY:-} || -z ${FIRMWARE_DIR:-} ]] || return 0
	local dev=${FIRMWARE_DEVICE:-starlte} top args=()
	if [[ -n ${FIRMWARE_DIR:-} ]]; then
		[[ -d $FIRMWARE_DIR ]] || { fw_say "FIRMWARE_DIR=$FIRMWARE_DIR is not a directory"; return 1; }
		FIRMWARE_DIR=$(cd "$FIRMWARE_DIR" && pwd)
		fw_check "$FIRMWARE_DIR" || { fw_say "ERROR: FIRMWARE_DIR=$FIRMWARE_DIR is not complete (above)"; return 1; }
	else
		top=$FW_SK/devices/$dev
		if [[ ! -x $top/tools/fetch-firmware.sh ]]; then
			fw_say "ERROR: no devices/$dev/tools/fetch-firmware.sh in $FW_SK: run 'python3 tools/hakios-device.py sync $dev'"
			fw_say "(it clones scion-$dev, which holds the firmware's sums), or set FIRMWARE_DIR to a firmware/ directory of yours"
			return 1
		fi
		[[ -z ${FIRMWARE_FROM:-} ]] || args+=(--from "$FIRMWARE_FROM")
		[[ ${FIRMWARE_OFFLINE:-0} != 1 ]] || args+=(--offline)
		fw_say "devices/$dev/tools/fetch-firmware.sh ${args[*]+${args[*]}}"
		if ! fw_as_builder "$top/tools/fetch-firmware.sh" ${args[@]+"${args[@]}"} >&2; then
			fw_say "ERROR: the firmware is not complete (above). FIRMWARE_FROM=DIR takes it from your own copy; the build"
			fw_say "stops rather than make an image without it (ENABLE_WIFI=0 ENABLE_AUDIO_BT=0 to build the root without both)"
			return 1
		fi
		FIRMWARE_DIR=$top/firmware
		# checked again here, by whoever runs this (the root build: as root, on the bytes it will copy)
		fw_check "$FIRMWARE_DIR" || { fw_say "ERROR: $FIRMWARE_DIR does not hold what its sums name (above)"; return 1; }
	fi
	FIRMWARE_READY=1
	export FIRMWARE_DIR FIRMWARE_READY
	fw_say "complete and checked: $FIRMWARE_DIR ($(fw_kinds | tr '\n' ' '| sed 's/ $//'))"
}

# fw_kinds : the kinds of FIRMWARE_DIR, one a line
fw_kinds() {
	local sf
	for sf in "$FIRMWARE_DIR"/*/SHA256SUMS; do [[ ! -f $sf ]] || basename "$(dirname "$sf")"; done
	return 0
}

# fw_files KIND : the file names KIND's sums name, one a line
fw_files() { awk '$1 !~ /^#/ && NF >= 2 { sub(/^\*/, "", $2); print $2 }' "$FIRMWARE_DIR/$1/SHA256SUMS"; }

# fw_summary ROOT : one line per kind, for the end of a root build; 1 if a feature that is on has its kind absent
fw_summary() {
	local r=$1 kind dir n have bad=0 on why f
	if [[ -z ${FIRMWARE_DIR:-} ]]; then
		echo "firmware: none prepared (both ENABLE_WIFI=0 and ENABLE_AUDIO_BT=0): the root has no Wi-Fi and no Bluetooth"
		return 0
	fi
	for kind in $(fw_kinds); do
		case $kind in
		wifi | bt)
			if [[ $kind == wifi ]]; then on=${ENABLE_WIFI:-1} why=ENABLE_WIFI dir=$FW_ROOT_DIR_wifi
			else on=${ENABLE_AUDIO_BT:-1} why=ENABLE_AUDIO_BT dir=$FW_ROOT_DIR_bt; fi
			n=0 have=0
			for f in $(fw_files "$kind"); do
				n=$((n + 1))
				[[ -f $r/$dir/$f ]] && grep -q "^$(fw_sha "$r/$dir/$f")  \*\{0,1\}$f\$" "$FIRMWARE_DIR/$kind/SHA256SUMS" && have=$((have + 1))
			done
			if [[ $on == 0 ]]; then
				echo "firmware: $kind: absent, on purpose ($why=0)$([[ $have -gt 0 ]] && echo ", but $have file(s) of it are on the root")"
			elif [[ $have == "$n" ]]; then
				echo "firmware: $kind: present and checked, $have of $n in /$dir"
			else
				echo "firmware: $kind: ABSENT, $have of $n in /$dir: its step did not put it there (its lines above: skipped or FAILED)"
				bad=1
			fi
			;;
		sensorhub)
			echo "firmware: sensorhub: checked in $FIRMWARE_DIR/sensorhub, not on the root: tools/ios27/s9/make-boot-image.sh puts it in the boot image's device tree" ;;
		*)
			echo "firmware: $kind: checked in $FIRMWARE_DIR/$kind, not put on the root: no driver of the iOS 27 kernel loads it" ;;
		esac
	done
	return $bad
}

# run by itself
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	set -euo pipefail
	case ${1:-} in
	prepare) fw_prepare; echo "$FIRMWARE_DIR" ;;
	check)
		if [[ -n ${FIRMWARE_DIR:-} ]]; then fw_check "$FIRMWARE_DIR"
		else fw_check "$FW_SK/devices/${FIRMWARE_DEVICE:-starlte}/firmware"; fi
		fw_say "ok: every file in place and right"
		;;
	summary)
		FIRMWARE_DIR=${FIRMWARE_DIR:-$FW_SK/devices/${FIRMWARE_DEVICE:-starlte}/firmware}
		fw_summary "${2:?usage: firmware.sh summary ROOT}"
		;;
	*) sed -n 3,7p "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
	esac
fi
