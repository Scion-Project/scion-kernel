#!/bin/bash
# verify-boot.sh SERIAL BOOT_IMG LABEL [WAIT_MIN=12] : ONE verification boot of the phone SERIAL (in TWRP, lock held by the caller).
# Flashes BOOT (serial-pinned, read-back verified), reboots, captures the USB console with epoch timestamps into work/boot-LABEL.log,
# after WAIT_MIN minutes prints tools/boot-metrics.py and ends the run with 'return' (-> TWRP), unless KEEP=1: then the capture is stopped,
# the console device is left to the caller (usb-shot.py / "touch X Y" / "button home" / "return") and its name is written to work/boot-LABEL.dev.
# Does NOT touch USERDATA.
set -uo pipefail
SERIAL=${1:?SERIAL}; BOOT=${2:?BOOT_IMG}; LABEL=${3:?LABEL}; WAIT=${4:-12}
H=$(cd "$(dirname "$0")" && pwd); ADB=${ADB:-adb}; A="$ADB -s $SERIAL"
LOG=$H/work/boot-$LABEL.log
[[ $($A get-state 2>/dev/null) == recovery ]] || { echo "$SERIAL not in TWRP"; exit 1; }
before=$(ls /dev/cu.usbmodem* 2>/dev/null | tr '\n' ' ')
N=$(stat -f %z "$BOOT"); L=$(shasum -a 256 "$BOOT" | cut -c1-64)
$A push "$BOOT" /tmp/boot.img >/dev/null && [[ $($A shell sha256sum /tmp/boot.img | cut -c1-64) == "$L" ]] || { echo "push mismatch"; exit 1; }
$A shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 2>/dev/null; sync"
[[ $($A shell "head -c $N /dev/block/by-name/BOOT | sha256sum" | cut -c1-64) == "$L" ]] || { echo "BOOT read-back mismatch"; exit 1; }
echo "FLASH-VERIFIED $L ($SERIAL) $(date -u +%T)"
$A reboot; T0=$(date +%s); echo "rebooted $(date -u +%T); waiting for the console"
DEV=
for i in $(seq 1 120); do
    for d in /dev/cu.usbmodem*; do [[ -e $d ]] || continue; case " $before " in *" $d "*) ;; *) DEV=$d;; esac; done
    [[ -n $DEV ]] && break; sleep 1
done
[[ -n $DEV ]] || { echo "no new usbmodem device within 120 s (before: $before)"; exit 1; }
echo "console $DEV (phone $SERIAL)"; : > "$LOG"
cat "$DEV" | perl -MTime::HiRes=time -ne 'BEGIN{$|=1} print sprintf("%.3f ", time), $_' >> "$LOG" &
CAT=$!
echo "$DEV" > "$H/work/boot-$LABEL.dev"
sleep $((WAIT*60))
if [[ ${KEEP:-0} == 1 ]]; then kill $CAT 2>/dev/null; else printf 'return\n' > "$DEV"; sleep 8; kill $CAT 2>/dev/null; fi
echo "run ended $(date -u +%T), boot started at epoch $T0"; python3 -I "$H/boot-metrics.py" "$LOG"
