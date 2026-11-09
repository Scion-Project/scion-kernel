#!/usr/bin/env python3
# fp16census.py CACHEDIR SUBSTR... : per image, counts of instruction classes the M3/A55 trap on
import sys, struct, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscsym   # the index: $DSCSYM_INDEX (see dscsym.py)
d = sys.argv[1]; want = sys.argv[2:]
c = dscsym.Cache(d)
p, f, h, maps = c.main
io, ic = struct.unpack_from('<II', h, 0x1c0)
f.seek(io); imgs = []
for _ in range(ic):
    a, mt, ino, po, _ = struct.unpack('<QQQII', f.read(32)); imgs.append((a, po))
CL = [  # (name, mask, value)
    ('fmov_h_imm', 0xFFE01FE0, 0x1EE01000), ('fmov_w_h', 0x7FFFFC00, 0x1EE60000), ('fmov_h_w', 0x7FFFFC00, 0x1EE70000),
    ('fcmp_h', 0xFFE0FC07, 0x1EE02000), ('fcmp_h_e/z', 0xFFE0FC07, 0x1EE02008), ('fcmpe_h', 0xFFE0FC17, 0x1EE02010),
    ('fp16_scalar_any', 0xFF200000, 0x1E200000 | (3 << 22)),
    ('fmlal_vec', 0xBF60FC00, 0x0E20EC00), ('fmlal2_vec', 0xBF60FC00, 0x2E20CC00), ('fmlal_el', 0xBFC0B400, 0x0F800000),
    ('fmlal2_el', 0xBFC0B400, 0x2F808000), ('fcadd', 0xBF20EC00, 0x2E00E400),
    ('sdot/udot', 0xBF20FC00, 0x0E009400), ('fjcvtzs', 0xFFFFFC00, 0x1E7E0000),
    ('fp16_vec_3same', 0x9F60C400, 0x0E400400),
]
for a, po in imgs:
    f.seek(po); s = f.read(300); path = s[:s.index(b'\0')].decode()
    if want and not any(w in path for w in want): continue
    hdr = c.read_va(a, 0x8000); ncmds = struct.unpack_from('<I', hdr, 16)[0]; off = 32; secs = []
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', hdr, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', hdr, off + 64)[0]
            for k in range(nsect):
                so = off + 72 + 80 * k
                sn = hdr[so:so + 16].rstrip(b'\0'); addr, size = struct.unpack_from('<QQ', hdr, so + 32)
                flags = struct.unpack_from('<I', hdr, so + 64)[0]
                if flags & 0x80000000: secs.append((sn, addr, size))
        off += sz
    cnt = {}
    for sn, addr, size in secs:
        w = np.frombuffer(c.read_va(addr, size)[:size // 4 * 4], dtype='<u4')
        for n, m, v in CL:
            k = int(np.count_nonzero((w & m) == v))
            if k: cnt[n] = cnt.get(n, 0) + k
    print(os.path.basename(path), cnt)
