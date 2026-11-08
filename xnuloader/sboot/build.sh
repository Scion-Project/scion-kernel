#!/bin/sh
# Build the starlte S-Boot Images: out/probe.Image (stage-0 handoff probe)
# and out/xnu_boot.Image (XNU loader).
# Based on ios-a5xelte shim/herolte/build.sh. No packaging or flashing.
# Any bare-metal or Linux aarch64 GCC works, e.g.
#   CROSS=aarch64-none-elf- sh build.sh
set -eu
cd "$(dirname "$0")"
: "${CROSS:=}"   # native gcc in the aarch64 VM; set aarch64-linux-gnu- elsewhere
mkdir -p out
CFLAGS="-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -fno-jump-tables -fno-tree-switch-conversion -fno-tree-loop-distribute-patterns -mgeneral-regs-only -mstrict-align -mcpu=cortex-a53 -Wall -Wextra -Werror"
# Menu v7 (docs/MENU7-PORT.md section 2): its renderer and menu use FP/SIMD (xnu_boot.S enables it
# for the loader and restores S-Boot's CPACR before an OS starts); every other file stays
# general-regs-only.
CFLAGS_V7="-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -fno-jump-tables -fno-tree-switch-conversion -fno-tree-loop-distribute-patterns -mstrict-align -mcpu=cortex-a55 -fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror"
# v7 is linked when all of its modules are in the tree; without them a stub keeps v6 the default.
if [ -f glass.c ] && [ -f sb7.c ] && [ -f intro7.c ] && [ -f menu7.c ]; then
    V7="$(ls glass*.c) menu7.c intro7.c sb7.c"
else
    cat > out/v7_off.c <<'V7OFF'
#include "../menu.h"
int menu7_ready(void) { return 0; }
int menu7_main(const struct star_info *in, char *c, unsigned n) { (void)in; (void)c; (void)n; return -1; }
void menu7_progress(unsigned k, unsigned n, const char *t, const char *d) { (void)k; (void)n; (void)t; (void)d; }
void menu7_handoff(const char *d) { (void)d; }
V7OFF
    V7="out/v7_off.c"
fi

image() {
    name=$1; shift
    objs=""
    for src in "$@"; do
        obj="out/${name}_$(basename "$src" | tr . _).o"
        case "$(basename "$src")" in
        glass*.c|menu7.c|intro7.c|sb7.c) flags=$CFLAGS_V7 ;;
        *) flags=$CFLAGS ;;
        esac
        "${CROSS}gcc" $flags "$src" -o "$obj"
        objs="$objs $obj"
    done
    "${CROSS}ld" -static -T image.ld --build-id=none --no-warn-rwx-segments $objs -o "out/$name.elf"
    # A raw Image must be position independent: no absolute relocations.
    if "${CROSS}readelf" -r $objs | grep -q 'ABS64'; then
        echo "$name: absolute relocation found" >&2
        exit 1
    fi
    "${CROSS}objcopy" -O binary "out/$name.elf" "out/$name.Image"
    end=$("${CROSS}nm" "out/$name.elf" | awk '$3 == "_image_end" { print $1 }')
    python3 - "out/$name.Image" "$end" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]).read_bytes()
assert p[56:60] == b'ARMd', 'missing arm64 Image magic'
# --append puts the payload at the 4 KiB-padded end, which must be _image_end.
assert (len(p) + 4095) // 4096 * 4096 == int(sys.argv[2], 16), '_image_end mismatch'
print(f'{sys.argv[1]}: {len(p)} bytes; arm64 Image header OK')
PY
}

image probe probe.S probe.c fbtext.c fdt.c
# USB device mode + loader fastboot (docs/USB-FASTBOOT.md)
USB="usb_dwc3.c usb_phy.c usb_desc.c max77705_muic.c fastboot.c fastboot_dev.c"
image xnu_boot xnu_boot.S xnu_boot.c mmu.c anim.c menu.c menu6.c ui.c em.c em_vec.S batt.c ufs.c store.c sd.c catalog.c fbtext.c fdt.c recovery.c post.c $USB $V7

# Engineer OS (docs/ENGINEER-OS.md): its own bare image on libsb, starbite/out/engineer.img. It is
# skipped (not an error) when the menu v7 asset container it embeds fonts from is missing.
if [ -f ../../starbite/engineer/build.sh ]; then
    sh ../../starbite/engineer/build.sh
fi
