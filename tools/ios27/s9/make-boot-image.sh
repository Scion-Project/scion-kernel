#!/bin/bash
# SPDX-License-Identifier: MIT-0
# make-boot-image.sh : the iOS 27 BOOT image for the Galaxy S9 (SM-G960F) from the three things you made yourself:
#   --kernel K      the kernel `OS_PROFILE=ios-27 make kernel` left (./hakios-kernel)
#   --donor-adt A   the iPhone 11 device tree of YOUR OWN IPSW (inputs/n104ap.adt, tools/ios27/s9/fetch-apple-inputs.sh)
#   --template T    an Android boot image that boots on YOUR phone: a copy of its RECOVERY partition with TWRP in it
#                   (adb pull /dev/block/by-name/RECOVERY: tools/ios27/s9/flash.sh pull-template), or the TWRP .img you installed.
#                   Its header, DTBH (Samsung's device tree for S-Boot) and nothing else are kept.
#   [--cmdline S | --cmdline-file F]   default tools/ios27/cmdline/starlte-rc.txt (the rest is built into the kernel)
#                   sweetepoch=<now> is added to it unless it names one (the clock's floor)
#   [--inject-record R | --inject-record none]   the record of the libraries the root build installed for the kernel to
#                   insert into programs (tools/ios27/fullroot/inject-record.sh). Default: tools/ios27/fullroot/out/root-full.img.inject
#                   when the root build of docs/ios27/INSTALL-S9.md left one, else none. With a record that lists libraries the
#                   line gets sweetinject= = the kernel's built-in three and exactly those (tools/ios27/s9/cmdline-inject.py, which
#                   refuses a line that would not fit or names sweetinject= itself); without one, or with none, the line is
#                   packed as given. Make the boot image again after every root build: the two must agree.
#   [--camera-node auto|yes|no]   the device tree's /arm-io/isp, by which iOS shows the Camera app (tools/ios27/camera/
#                   adt-isp-node.py). auto (the default): only when the line, with the kernel's built-in arguments, has cam_s9=7
#                   and the record names the root's /usr/lib/cam.dylib for cameracaptured (step 94-camera); without both the
#                   tree is as before and the app hidden. yes: always; no: never.
#   [--firmware auto|none]   the sensor hub's image (bcm_sensorhub_a1.bin), which the kernel loads before there is a root, in
#                   the device tree's /scion-firmware (tools/ios27/adt-firmware.py; docs/ios27/SENSORS.md). auto (the default):
#                   the device's firmware made complete and checked by tools/ios27/fullroot/firmware.sh (FIRMWARE_FROM=DIR,
#                   FIRMWARE_OFFLINE=1, FIRMWARE_DIR=DIR as there), and a file missing or wrong stops this; none: a tree
#                   without it (shub=4 and up then find no image: no sensors, no rotation). The image then carries
#                   Broadcom's and Samsung's file: it is private anyway (below).
#   [--canvas WxH]  the screen iOS draws (the device tree's /arm-io/disp0 canvas, mkadt_starlte.py --canvas). Default: the
#                   iPhone 11's own 828x1792. The S9's panel shape is 828x1702 (docs/ios27/PRESENT.md): with it the line must
#                   say the same (sweetfb_lw=828 sweetfb_lh=1702), and this refuses a line whose sweetfb_lw/sweetfb_lh differ.
#   [--loader I]    default xnuloader/sboot/out/xnu_boot.Image (tools/ios27/s9/build-loader.sh)
#   [--out O]       default out/xnu-boot-ios27.img (out/ is not in git; the image holds your IPSW's device identity: never share it)
#   [--loader-kind sboot|scion]   sboot (the default, unchanged): the loader above with the payload appended. scion: the
#                   second-stage loader of Scion-Project/scion-boot-exynos9810 (loader/, docs/STATUS.md there), which
#                   checks the SHA-256 of what it starts and has a menu, a boot-loop guard and a crash screen; NOT YET RUN
#                   ON A PHONE (docs/ios27/SCION-LOADER.md). The payload becomes its default entry "iOS 27".
#   [--scion-boot D]   scion: a checkout of scion-boot-exynos9810 where `make PLATFORM=starlte loader` has run
#                   (D/build/starlte/scion-loader.bin, D/tools/mkloaderimg.py)
#   [--scion-entry NAME:xnu:FILE[:safe]]   scion: one more entry (repeatable), e.g. an iOS 16.5 payload or a known-good
#                   build marked safe (the boot-loop guard starts it after three failed boots)
#   [--scion-chain-sboot]   scion: also append the sboot loader above with this payload as the entry "Starbite" (its menus,
#                   store, recovery and fastboot one menu entry away; the payload is then in BOOT twice)
#   [--payload P]   pack an existing SWEETXNU payload exactly as it is instead of making one: no --kernel, --donor-adt,
#                   --cmdline, --inject-record, --camera-node, --firmware or --canvas, no sweetepoch= added; the payload's own
#                   command line is used as it stands. For a new loader around the payload of an image that already boots
#                   (`starlte-pack-boot.py payload BOOTIMG OUT` takes it out of one). Its line is not printed.
# What it does (the steps the project's test images are made with): strip -x copy of the kernel; the S9's Apple device tree
# (mkadt_starlte.py --big --bank2 --bank2-mb 1536 --iphone11, then adt-extra-ram.py: 8 cores, the 1.5 GiB window of the second DRAM
# bank, seven extra RAM ranges of the first; adt-firmware.py, see --firmware; camera/adt-isp-node.py, see --camera-node); mkpayload.py (kernel + ADT + command line -> SWEETXNU payload, padded to 4 KiB);
# starlte-pack-boot.py (the loader as the image's kernel, the payload appended, a one-page empty ramdisk).
# With --loader-kind scion the last step differs: scion-boot's mkloaderimg.py makes the kernel slot (the loader, then each
# entry as an SCN2 record with its SHA-256), starlte-pack-boot.py packs it with --image-size-from-file.
# Nothing here touches a phone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); T27=$ROOT/tools/ios27
PY=${PY:-python3}
KERNEL= DONOR= TEMPLATE= LINE= OUT=$ROOT/out/xnu-boot-ios27.img LOADER=$ROOT/xnuloader/sboot/out/xnu_boot.Image
LINEFILE=$T27/cmdline/starlte-rc.txt
RECORD=default CAMERA_NODE=auto FIRMWARE=auto CANVAS=
LOADER_KIND=sboot SCION_BOOT= SCION_CHAIN=0 SCION_ENTRIES=() PAYLOAD=
while [ $# -gt 0 ]; do
	case "$1" in
	--kernel) KERNEL=$2; shift ;; --donor-adt) DONOR=$2; shift ;; --template) TEMPLATE=$2; shift ;;
	--cmdline) LINE=$2; LINEFILE=; shift ;; --cmdline-file) LINEFILE=$2; shift ;;
	--loader) LOADER=$2; shift ;; --out) OUT=$2; shift ;; --inject-record) RECORD=$2; shift ;;
	--camera-node) CAMERA_NODE=$2; shift ;;
	--firmware) FIRMWARE=$2; shift ;;
	--canvas) CANVAS=$2; shift ;;
	--loader-kind) LOADER_KIND=$2; shift ;;
	--scion-boot) SCION_BOOT=$2; shift ;;
	--scion-entry) SCION_ENTRIES+=("$2"); shift ;;
	--scion-chain-sboot) SCION_CHAIN=1 ;;
	--payload) PAYLOAD=$2; shift ;;
	-h | --help) sed -n '3,52p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "make-boot-image: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
die() { echo "make-boot-image: $*" >&2; exit 1; }
if [ -n "$PAYLOAD" ]; then
	[ -f "$PAYLOAD" ] || die "--payload: no file at '$PAYLOAD'"
	[ -z "$KERNEL$DONOR" ] || die "--payload replaces --kernel and --donor-adt: give one or the other"
	[ "$(head -c 8 "$PAYLOAD")" = SWEETXNU ] || die "--payload: $PAYLOAD is not a SWEETXNU payload"
else
	[ -f "$KERNEL" ] || die "--kernel: no kernel at '$KERNEL' (OS_PROFILE=ios-27 make kernel)"
	[ -f "$DONOR" ] || die "--donor-adt: no device tree at '$DONOR' (tools/ios27/s9/fetch-apple-inputs.sh)"
fi
[ -f "$TEMPLATE" ] || die "--template: no boot image at '$TEMPLATE' (tools/ios27/s9/flash.sh pull-template)"
case "$LOADER_KIND" in
sboot)
	[ -z "$SCION_BOOT" ] && [ "$SCION_CHAIN" = 0 ] && [ ${#SCION_ENTRIES[@]} = 0 ] ||
		die "--scion-boot, --scion-entry and --scion-chain-sboot go with --loader-kind scion"
	[ -f "$LOADER" ] || die "no loader at $LOADER (tools/ios27/s9/build-loader.sh)" ;;
scion)
	SL=$SCION_BOOT/build/starlte/scion-loader.bin
	[ -n "$SCION_BOOT" ] && [ -f "$SL" ] && [ -f "$SCION_BOOT/build/starlte/scion-loader.image-end" ] &&
		[ -f "$SCION_BOOT/tools/mkloaderimg.py" ] ||
		die "--loader-kind scion: --scion-boot DIR, a scion-boot-exynos9810 checkout after make PLATFORM=starlte loader"
	[ "$SCION_CHAIN" = 0 ] || [ -f "$LOADER" ] || die "--scion-chain-sboot: no loader at $LOADER (tools/ios27/s9/build-loader.sh)" ;;
*) die "--loader-kind sboot or scion (got '$LOADER_KIND')" ;;
esac
case "$CANVAS" in '' | [1-9]*x[1-9]*) ;; *) die "--canvas WxH (got '$CANVAS')" ;; esac
case "$CANVAS" in *[!0-9x]* | *x*x*) die "--canvas WxH (got '$CANVAS')" ;; esac
mkdir -p "$(dirname "$OUT")"
W=$(mktemp -d "${TMPDIR:-/tmp}/bootimg.XXXXXX"); trap 'rm -rf "$W"' EXIT
if [ -n "$PAYLOAD" ]; then
	"$PY" - "$PAYLOAD" "$W/payload.bin" <<'PY'
import hashlib, sys
from pathlib import Path
b = Path(sys.argv[1]).read_bytes()
line = b[0x48:0x48 + 608]
assert b[:8] == b'SWEETXNU' and len(b) > 0x2a8, 'not a SWEETXNU payload'
assert line[607] == 0, 'the payload\'s command line is not terminated'
n = len(line.split(b'\0')[0])
Path(sys.argv[2]).write_bytes(b + b'\0' * (-len(b) % 4096))
print(f'make-boot-image: --payload: {len(b)} bytes, sha256 {hashlib.sha256(b).hexdigest()}; its command line {n} of 607 bytes '
      f'(sha256 {hashlib.sha256(line.split(bytes(1))[0]).hexdigest()}), used as it is')
PY
	n=; LINE=
else
# the sensor hub's image for the device tree (firmware.sh: complete and checked, or no image)
SHUB_FILES=
case "$FIRMWARE" in
auto)
	. "$T27/fullroot/firmware.sh"
	fw_prepare || die "the S9's firmware is not complete (above): --firmware none packs a tree without the sensor hub's image"
	for f in $(fw_files sensorhub); do SHUB_FILES="$SHUB_FILES $FIRMWARE_DIR/sensorhub/$f"; done
	[ -n "$SHUB_FILES" ] || die "$FIRMWARE_DIR/sensorhub/SHA256SUMS names no file"
	;;
none) echo "make-boot-image: --firmware none: no sensor hub image in the device tree (no sensors)" ;;
*) die "--firmware auto or none (got '$FIRMWARE')" ;;
esac
if [ -n "$LINEFILE" ]; then [ -f "$LINEFILE" ] || die "no command line file $LINEFILE"; LINE=$(cat "$LINEFILE"); fi
case "$LINE" in *$'\n'*) die "the command line has more than one line" ;; esac
# the calendar's floor: the time this image was made, unless the line has its own (the kernel's clock starts there until the
# PMIC's clock or the network says better; without it every boot starts in 1970)
case " $LINE " in *" sweetepoch="*) ;; *) LINE="$LINE sweetepoch=$(date +%s)" ;; esac
# the libraries the root has for insertion (sweetinject=), from the root build's record
if [ "$RECORD" = default ]; then
	RECORD=none; DEF=$ROOT/tools/ios27/fullroot/out/root-full.img.inject
	[ ! -f "$DEF" ] || { RECORD=$DEF; echo "make-boot-image: the root build's record $DEF (--inject-record none: without it)"; }
fi
if [ "$RECORD" != none ]; then
	[ -f "$RECORD" ] || die "--inject-record: no record at '$RECORD' (tools/ios27/fullroot/inject-record.sh)"
	LINE=$("$PY" "$T27/s9/cmdline-inject.py" compose --line "$LINE" --record "$RECORD" --kernel "$KERNEL") || die "the record $RECORD cannot go on this command line (above)"
fi
# the canvas and the framebuffer size the kernel is given must agree (it scales sweetfb_lw x sweetfb_lh to the panel): the
# line's value, else the kernel's built-in one (osfmk/arm/arm_init.c: sweetfb_lw=828 sweetfb_lh=1792)
if [ -n "$CANVAS" ]; then
	for kv in "sweetfb_lw=${CANVAS%x*}=828" "sweetfb_lh=${CANVAS#*x}=1792"; do
		k=${kv%%=*}; want=${kv#*=}; want=${want%=*}; builtin=${kv##*=}
		got=$(printf ' %s ' "$LINE" | tr ' ' '\n' | sed -n "s/^$k=//p" | tail -1)
		[ "${got:-$builtin}" = "$want" ] || die "--canvas $CANVAS needs $k=$want (the line gives ${got:-nothing, so the built-in $builtin applies})"
	done
fi
n=$(printf '%s' "$LINE" | wc -c | tr -d ' ')
[ "$n" -le 607 ] || die "the command line is $n bytes; the payload holds 607 (it would be cut)"
cp "$KERNEL" "$W/kernel.x"; xcrun strip -x "$W/kernel.x"
"$PY" "$T27/mkadt_starlte.py" --big --bank2 --bank2-mb 1536 --iphone11 "$DONOR" ${CANVAS:+--canvas "$CANVAS"} "$W/base.adt" >/dev/null
"$PY" "$T27/adt-extra-ram.py" "$W/base.adt" "$W/ram.adt"
if [ -n "$SHUB_FILES" ]; then
	"$PY" "$T27/adt-firmware.py" "$W/ram.adt" "$W/fw.adt" $SHUB_FILES | sed 's/^/make-boot-image: \/scion-firmware: /'
	mv -f "$W/fw.adt" "$W/ram.adt"
fi
# the camera's /arm-io/isp (the Camera app shown) only when this line asks the kernel for the stream (cam_s9=7) and the
# root's record has the capture device (step 94-camera): tools/ios27/camera/adt-isp-node.py, docs/ios27/CAMERA.md
"$PY" "$T27/camera/adt-isp-node.py" "$W/ram.adt" "$W/starlte.adt" --when "$CAMERA_NODE" --line "$LINE" --kernel "$KERNEL" \
	$([ "$RECORD" = none ] || printf -- '--record %s' "$RECORD")
"$PY" "$T27/herolte/mkpayload.py" "$W/kernel.x" "$W/starlte.adt" "$W/payload.bin" --cmdline "$LINE" | grep -E "^(size|fixups|entry)" || true
"$PY" - "$W/payload.bin" "$LINE" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); b = p.read_bytes()
assert b[:8] == b'SWEETXNU', 'not a SWEETXNU payload'
packed = b[0x48:0x48 + 608].split(b'\0')[0].decode()
if packed != sys.argv[2]:
    sys.exit('the packed command line differs from the one given')
p.write_bytes(b + b'\0' * (-len(b) % 4096))
print(f'command line: {len(packed)} of 607 bytes, read back from the payload')
PY
fi
head -c 4096 /dev/zero > "$W/ramdisk.bin"
if [ "$LOADER_KIND" = sboot ]; then
	"$PY" "$T27/starlte-pack-boot.py" pack "$TEMPLATE" "$LOADER" "$OUT" --append "$W/payload.bin" --ramdisk "$W/ramdisk.bin" | tail -1
else
	# scion-boot's loader: the payload as its default entry, then --scion-entry ones, then (asked) sboot with the payload;
	# everything must end below S-Boot's initrd at 0x89000000 (load at 0x80080000)
	ENTRIES=(--entry "iOS 27:xnu:$W/payload.bin:default")
	for e in ${SCION_ENTRIES[@]+"${SCION_ENTRIES[@]}"}; do ENTRIES+=(--entry "$e"); done
	if [ "$SCION_CHAIN" = 1 ]; then
		"$PY" "$T27/starlte-pack-boot.py" pack "$TEMPLATE" "$LOADER" "$W/sboot.img" --append "$W/payload.bin" --ramdisk "$W/ramdisk.bin" >/dev/null
		"$PY" "$T27/starlte-pack-boot.py" kernel "$W/sboot.img" "$W/sboot-slot.bin" >/dev/null
		ENTRIES+=(--entry "Starbite:image:$W/sboot-slot.bin")
	fi
	"$PY" "$SCION_BOOT/tools/mkloaderimg.py" --loader "$SL" --image-end "$(cat "$SCION_BOOT/build/starlte/scion-loader.image-end")" \
		"${ENTRIES[@]}" --max-bytes 0x8f80000 -o "$W/slot.bin" | sed 's/^/make-boot-image: /'
	"$PY" "$T27/starlte-pack-boot.py" pack "$TEMPLATE" "$W/slot.bin" "$OUT" --ramdisk "$W/ramdisk.bin" --image-size-from-file | tail -1
	echo "make-boot-image: scion-boot loader: NOT YET RUN ON A PHONE (docs/ios27/SCION-LOADER.md: the first test, and the way back)"
fi
shasum -a 256 "$OUT" | tee "$OUT.sha256"
[ -n "$PAYLOAD" ] || echo "command line ($n bytes): $LINE"
