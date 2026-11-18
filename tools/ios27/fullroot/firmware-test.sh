#!/bin/bash
# SPDX-License-Identifier: MIT-0
# firmware-test.sh : the S9's firmware in the iOS 27 root build (firmware.sh), with made-up files in place of the vendor's, in a
# directory of its own (removed at the end): a complete, checked directory is taken and exported; a file missing or with a
# wrong sum stops it with the file named; a device repository's fetch-firmware.sh that fails stops it too (the root build then
# makes no root); the Wi-Fi's files go onto a scratch root and come off it again (tools/ios27/wifi/60-wifi-firmware.sh); the
# summary of a root says present for what is there and ABSENT, with a failing status, for what a feature that is on lacks;
# the sensor hub's file goes into a device tree (tools/ios27/adt-firmware.py). CI runs it (Linux); it runs on macOS too.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); SK=$(cd "$H/../../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/fwtest.XXXXXX"); trap 'rm -rf "$T"' EXIT
T=$(cd "$T" && pwd)     # the form firmware.sh gives back (macOS's TMPDIR ends in a /)
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
if command -v shasum >/dev/null 2>&1; then sha() { shasum -a 256 "$1" | cut -c1-64; }; else sha() { sha256sum "$1" | cut -c1-64; }; fi

# a made-up firmware/ directory: three kinds, the sums of made-up bytes
mkfw() { # DIR
	local d=$1 k f
	for k in wifi bt sensorhub audio; do mkdir -p "$d/$k"; : > "$d/$k/SHA256SUMS"; done
	for f in bcmdhd_sta.bin_b2 bcmdhd_clm.blob nvram.txt_r02a_b2; do echo "made-up $f" > "$d/wifi/$f"; done
	for f in bcm4361B2_murata.hcd bcm4361B2_semco.hcd; do echo "made-up $f" > "$d/bt/$f"; done
	echo "made-up hub" > "$d/sensorhub/bcm_sensorhub_a1.bin"
	echo "made-up dsp" > "$d/audio/calliope_sram.bin"
	for k in wifi bt sensorhub audio; do
		for f in "$d/$k"/*; do [[ $(basename "$f") == SHA256SUMS ]] || echo "$(sha "$f")  $(basename "$f")" >> "$d/$k/SHA256SUMS"; done
	done
}

# 1. FIRMWARE_DIR: complete -> taken; a file missing, a wrong sum -> refused, the file named
mkfw "$T/fw"
out=$( (. "$H/firmware.sh"; export FIRMWARE_DIR=$T/fw; fw_prepare && echo "DIR=$FIRMWARE_DIR READY=$FIRMWARE_READY") 2>&1) || true
[[ $out == *"DIR=$T/fw READY=1"* ]] && ok "a complete directory is taken and exported" || bad "complete directory: $out"
cp -R "$T/fw" "$T/fw-missing"; rm "$T/fw-missing/wifi/bcmdhd_clm.blob"
if out=$( (. "$H/firmware.sh"; FIRMWARE_DIR=$T/fw-missing fw_prepare) 2>&1); then bad "a missing file was taken"
else [[ $out == *"missing: $T/fw-missing/wifi/bcmdhd_clm.blob"* ]] && ok "a missing file stops it, named" || bad "missing file: $out"; fi
cp -R "$T/fw" "$T/fw-wrong"; echo changed >> "$T/fw-wrong/bt/bcm4361B2_semco.hcd"
if out=$( (. "$H/firmware.sh"; FIRMWARE_DIR=$T/fw-wrong fw_prepare) 2>&1); then bad "a wrong file was taken"
else [[ $out == *"WRONG SUM: $T/fw-wrong/bt/bcm4361B2_semco.hcd"* ]] && ok "a wrong sum stops it, named" || bad "wrong sum: $out"; fi

# 2. the device repository's fetch-firmware.sh: run, and its failure stops it (a checkout of its own, made-up)
mkdir -p "$T/sk/tools/ios27/fullroot" "$T/sk/devices/fake/tools"
cp "$H/firmware.sh" "$T/sk/tools/ios27/fullroot/"
cat > "$T/sk/devices/fake/tools/fetch-firmware.sh" <<EOF
#!/bin/bash
# made up: copies $T/fw into place, or fails when FAKE_FAIL=1
[ "\${FAKE_FAIL:-0}" = 1 ] && { echo "fetch-firmware: ERROR: made-up failure" >&2; exit 1; }
mkdir -p "\$(dirname "\$0")/../firmware" && cp -R "$T/fw/." "\$(dirname "\$0")/../firmware/"
EOF
chmod +x "$T/sk/devices/fake/tools/fetch-firmware.sh"
if out=$( (. "$T/sk/tools/ios27/fullroot/firmware.sh"; FIRMWARE_DEVICE=fake FAKE_FAIL=1 fw_prepare) 2>&1); then bad "a failed fetch was taken"
else [[ $out == *"the build"*"stops"* ]] && ok "a failed fetch stops it" || bad "failed fetch: $out"; fi
out=$( (. "$T/sk/tools/ios27/fullroot/firmware.sh"; FIRMWARE_DEVICE=fake fw_prepare && echo "DIR=$FIRMWARE_DIR") 2>&1) || true
[[ $out == *"DIR=$T/sk/devices/fake/firmware"* ]] && ok "fetched into the device repository and checked" || bad "fetch: $out"
if out=$( (. "$T/sk/tools/ios27/fullroot/firmware.sh"; FIRMWARE_DEVICE=nothere fw_prepare) 2>&1); then bad "no device repository was taken"
else [[ $out == *"hakios-device.py sync nothere"* ]] && ok "no device repository: says to sync it" || bad "no repository: $out"; fi

# 3. the Wi-Fi's files onto a scratch root and off it again; the summary of that root
R=$T/root; mkdir -p "$R/private/var"
WIFI_FW_DIR=$T/fw/wifi WIFI_FW_SUMS=$T/fw/wifi/SHA256SUMS bash "$SK/tools/ios27/wifi/60-wifi-firmware.sh" "$R" >/dev/null 2>&1
n=$(ls "$R/private/var/hakios/wifi" | wc -l | tr -d ' ')
[[ $n == 3 ]] && cmp -s "$T/fw/wifi/bcmdhd_clm.blob" "$R/private/var/hakios/wifi/bcmdhd_clm.blob" && ok "the Wi-Fi's 3 files on the root" || bad "Wi-Fi on the root: $n files"
out=$( (. "$H/firmware.sh"; FIRMWARE_DIR=$T/fw FIRMWARE_READY=1 fw_summary "$R"; echo "status=$?") 2>&1) || true
[[ $out == *"wifi: present and checked, 3 of 3"* && $out == *"bt: ABSENT, 0 of 2"* && $out == *"sensorhub: checked"* &&
   $out == *"audio: checked"*"not put on the root"* && $out == *"status=1"* ]] && ok "summary: wifi present, bt ABSENT, status 1" || bad "summary: $out"
out=$( (. "$H/firmware.sh"; FIRMWARE_DIR=$T/fw FIRMWARE_READY=1 ENABLE_AUDIO_BT=0 fw_summary "$R"; echo "status=$?") 2>&1) || true
[[ $out == *"bt: absent, on purpose (ENABLE_AUDIO_BT=0)"* && $out == *"status=0"* ]] && ok "summary: bt off on purpose, status 0" || bad "summary off: $out"
echo changed >> "$R/private/var/hakios/wifi/nvram.txt_r02a_b2"
out=$( (. "$H/firmware.sh"; FIRMWARE_DIR=$T/fw FIRMWARE_READY=1 ENABLE_AUDIO_BT=0 fw_summary "$R"; echo "status=$?") 2>&1) || true
[[ $out == *"wifi: ABSENT, 2 of 3"* && $out == *"status=1"* ]] && ok "summary: a changed file on the root is not counted" || bad "summary changed: $out"
bash "$SK/tools/ios27/wifi/60-wifi-firmware.sh" "$R" undo >/dev/null
[[ ! -e $R/private/var/hakios ]] && ok "undo: the Wi-Fi's files off the root, nothing left" || bad "undo left $(ls -R "$R/private/var")"

# 4. the sensor hub's file into a device tree: a made-up one, a root node with a name and no children
python3 -I - "$T/plain.adt" <<'PY'
import struct, sys
name = b'name'.ljust(32, b'\0'); val = b'device-tree\0'
blob = struct.pack('<II', 1, 0) + name + struct.pack('<I', len(val)) + val + b'\0' * (-len(val) % 4)
open(sys.argv[1], 'wb').write(blob)
PY
if python3 "$SK/tools/ios27/adt-firmware.py" "$T/plain.adt" "$T/fw.adt" "$T/fw/sensorhub/bcm_sensorhub_a1.bin" >/dev/null &&
   python3 -I - "$T/fw.adt" "$T/fw/sensorhub/bcm_sensorhub_a1.bin" <<'PY'
import sys
blob = open(sys.argv[1], 'rb').read(); want = open(sys.argv[2], 'rb').read()
sys.exit(0 if b'scion-firmware\0' in blob and want in blob and b'bcm_sensorhub_a1.bin\0' in blob else 1)
PY
then ok "the sensor hub's file in the device tree's /scion-firmware"; else bad "adt-firmware"; fi
if python3 "$SK/tools/ios27/adt-firmware.py" "$T/fw.adt" "$T/fw2.adt" "$T/fw/sensorhub/bcm_sensorhub_a1.bin" >/dev/null 2>&1; then
	bad "a second /scion-firmware was added"; else ok "a tree that has /scion-firmware is refused"; fi

[[ $fail == 0 ]] && echo "firmware-test: all passed" || { echo "firmware-test: FAILED"; exit 1; }
