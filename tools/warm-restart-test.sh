#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The warm restart's re-entry record (docs/ios27/WARM-RESTART.md): xnuloader/sboot/warm_record.c, the loader's checks
# of the record a running kernel leaves it and of the payload's SHA-256, and tools/warm_slot.c, the sizes of BOOT's
# kernel slot as the XNU fork's bsd/dev/starlte_warm.c takes them, built for the host with tools/warm_restart_test.c
# under the address and undefined-behaviour sanitizers, and run: once on its own cases, once on a BOOT image that
# tools/ios27/starlte-pack-boot.py makes from a made-up template and loader (no private file: the template has the
# TWRP kernel's Image header shape, image_size 0x2842000, which the packer copies; the slot is about 20 MB, as the
# phone's, so the header's image_size is larger than the slot, as on every image the packer makes).
# usage: tools/warm-restart-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$tmp/warm-restart-test" tools/warm_restart_test.c tools/warm_slot.c xnuloader/sboot/warm_record.c
"$tmp/warm-restart-test"

python3 - "$tmp" <<'PY'
import struct, sys
from pathlib import Path
t = Path(sys.argv[1])
def image(size, image_size, mark):
    b = bytearray(size)
    struct.pack_into("<3Q", b, 8, 0x80000, image_size, 0xa)
    b[56:60] = b"ARMd"
    if mark:
        b[0x44:0x4c] = b"SCNR" + struct.pack("<I", 1)
    return b
# the template: an Android v0 header (page 2048), an arm64 Image with the TWRP kernel's image_size, a ramdisk, a DTBH
k, r, dt = image(0x8000, 0x2842000, False), bytes(4096), b"DTBH" + bytes(2044)
h = bytearray(2048)
h[:8] = b"ANDROID!"
struct.pack_into("<10I", h, 8, len(k), 0x10008000, len(r), 0x11000000, 0, 0x10f00000, 0x10000100, 2048, len(dt), 0)
(t / "template.img").write_bytes(bytes(h) + k + r + dt)
# the loader: 1.6 MiB with the warm entry's mark; the payload: a SWEETXNU header and 18 MiB of something
(t / "loader.Image").write_bytes(image(0x196598, 0, True))
p = bytearray(18 << 20)
p[:8] = b"SWEETXNU"
struct.pack_into("<II", p, 8, 1, 1)
for i in range(0x1000, len(p), 4096):
    p[i:i + 8] = struct.pack("<Q", i * 2654435761 & (2**64 - 1))
(t / "payload").write_bytes(p)
PY
python3 tools/ios27/starlte-pack-boot.py pack "$tmp/template.img" "$tmp/loader.Image" "$tmp/boot.img" \
	--append "$tmp/payload" > "$tmp/pack.log"
"$tmp/warm-restart-test" "$tmp/boot.img" 0x197000
