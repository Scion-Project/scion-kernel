#!/usr/bin/env bash
# Turn a STARLTE kernel into out/xnu-boot.img for the BOOT partition:
# starlte ADT + SWEETXNU payload appended to shim/starlte xnu_boot.Image,
# in the S-Boot layout of the TWRP template. Nothing touches the phone.
#   tools/make-xnu-boot.sh [KERNEL]              (default out/kernel.development.starlte)
#   CMDLINE="..." tools/make-xnu-boot.sh          override the boot-args
set -euo pipefail
cd "$(dirname "$0")/.."
kernel=${1:-out/kernel.development.starlte}
[ -f "$kernel" ] || { echo "no kernel at $kernel (gh run download ... -n xnu-kernel -D out)"; exit 1; }
# One CPU, verbose console on the framebuffer, read-only UFS probe (GPT dump).
: "${CMDLINE:=debug=0xc serial=3 cpus=1 keepsyms=1 -v -enable_kprintf_spam -noprogress wdt=-1 -wdt_no_pmu ufsprobe=1}"

limactl shell starbite -- sh "$PWD/shim/starlte/build.sh" >/dev/null
# ADTFLAGS=--scale2 for the scaled iPhone XR canvas (with sweetfb_scale boot-args).
python3 tools/mkadt_starlte.py ${ADTFLAGS:-} out/starlte.adt
python3 ref/ios-herolte/tools/mkpayload.py "$kernel" out/starlte.adt out/payload.bin --cmdline "$CMDLINE"
# SPLASH=build/brand/overlay.rle (tools/brand/rle.py; .bin works too, 6 MB more):
# SPLASH=build/brand/splash.bin: a boot splash in the ramdisk slot (to-rgb565.py),
# drawn by the loader.
# LINUX=Image: a Linux kernel bundle, and
# UEFI=build/woa/uefi-22621.7582.bin: the Windows UEFI (tools/woa/get-uefi.sh),
# appended after the SWEETXNU payload (tools/store/bundle.py, xnu_boot.c
# star_linux / star_uefi).
append=out/payload.bin
if [ -n "${LINUX:-}${UEFI:-}" ]; then
    python3 tools/store/bundle.py out/payload.bin out/payload-linux.bin \
        ${LINUX:+--linux "$LINUX"} ${UEFI:+--uefi "$UEFI"}
    append=out/payload-linux.bin
fi
python3 tools/starlte-pack-boot.py pack device/twrp-3.7.0_9-0-starlte.img \
    shim/starlte/out/xnu_boot.Image out/xnu-boot.img --append "$append" \
    ${SPLASH:+--ramdisk "$SPLASH"}
