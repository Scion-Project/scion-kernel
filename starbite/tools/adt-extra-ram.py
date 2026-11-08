#!/usr/bin/env python3
"""Add /chosen "hakios-extra-ram" (u64 base, u64 size pairs) to a Starbite STARLTE ADT, for the kernel's extra RAM
(port of Scion patch 1512; kernel boot-arg extra_ram=1). Optionally move the window's dram-size (--dram-mb).

  adt-extra-ram.py STARBITE_DIR IN.adt OUT.adt [--dram-mb N]

Ranges: the S9's low DRAM bank (TWRP /proc/iomem: 0x80000000-0xBC7FFFFF, 0xC0000000-0xDFFFFFFF, 0xE1900000-0xFF7FFFFF),
less, on the S-Boot -> Starbite shim -> XNU path:
  0x80000000 + 2 MiB      sec_debug_magic (DT) and a margin
  0x8F000000-0x91FFFFFF   SMP CPU_ON trampoline 0x8fffe000 / mailbox 0x8ffff000 (SweetCPU.cpp), herolte_gpu scratch
                          0x8fe00000, zswap_debug 0x91300000 (DT), S-Boot's cmdline log areas 0x91202000-0x9179c000,
                          the shim's log page 0x91fff000
  0xAF800000 + 32 MiB     rkp_region (DT)
  0xB8000000 + 2 MiB      tima_region (DT)
  0xBC800000-0xBFFFFFFF   not RAM to Linux (iomem hole: secure)
  0xC0000000 + 512 KiB    seclog_mem (DT)
  0xCC000000-0xCFFFFFFF   the panel's buffer (DECON scans it; Mali maps it: starlte_gpu.c SG_FB_PA) and a margin
  0xE0000000-0xE36FFFFF   iomem hole + secure_camera (DT)
  0xF0000000 and up       modem_if, cp_ram_logging, abox, exynos_ss (pstore pmsg 0xFED14000 used by herolte_ufs.c), fimc
Everything else there is only used by S-Boot and the shim before XNU starts (S-Boot's FDT 0x8a000000, initrd 0x89000000,
the shim's STAGE 0x8c000000-0x91000000 and its RAM window 0x92000000-0xaf800000).
"""
import argparse
import struct
import sys
from pathlib import Path

RANGES = [
    (0x80200000, 0x8F000000),
    (0x92000000, 0xAF800000),
    (0xB1800000, 0xB8000000),
    (0xB8200000, 0xBC800000),
    (0xC0080000, 0xCC000000),
    (0xD0000000, 0xD8000000),   # 0xD8000000-0xDFFFFFFF: the display's carve-out (sys/hakios.exynos9810_mem.h)
    (0xE3700000, 0xF0000000),
]

ap = argparse.ArgumentParser()
ap.add_argument('starbite')
ap.add_argument('inp')
ap.add_argument('out')
ap.add_argument('--dram-mb', type=int, default=0, help='also set dram-size (the bank-2 window) to N MiB')
a = ap.parse_args()
sys.path.insert(0, str(Path(a.starbite) / 'ref/ios-herolte/tools'))
import adt  # noqa: E402

root, _ = adt.parse(Path(a.inp).read_bytes())
chosen = next(c for c in root.children if c.name == 'chosen')
base, = struct.unpack('<Q', chosen.prop('dram-base'))
size, = struct.unpack('<Q', chosen.prop('dram-size'))
if a.dram_mb:
    size = a.dram_mb << 20
    assert base == 0x880000000 and base + size <= 0x8F0000000, 'window would reach the GPU memory at 0x8f0000000'
    chosen.props = [(k, f, struct.pack('<Q', size) if k == 'dram-size' else v) for k, f, v in chosen.props]
for s, e in RANGES:
    assert s % 0x4000 == 0 and e % 0x4000 == 0 and e > s and e <= base, hex(s)
value = b''.join(struct.pack('<QQ', s, e - s) for s, e in RANGES)
chosen.props = [p for p in chosen.props if p[0] != 'hakios-extra-ram']
chosen.props.append(('hakios-extra-ram', 0, value))
Path(a.out).write_bytes(adt.serialize(root))
extra = sum(e - s for s, e in RANGES)
print(f'window {base:#x}+{size >> 20} MiB, extra {extra >> 20} MiB in {len(RANGES)} ranges, total {(size + extra) >> 20} MiB')
