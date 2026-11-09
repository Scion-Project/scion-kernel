#!/usr/bin/env python3
# fp16sites.py CACHEDIR IMAGE_SUBSTR : list FP16 vector/FHM sites (pc, word, function) of one image
import sys, struct, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscsym   # the index: $DSCSYM_INDEX (see dscsym.py)
c = dscsym.Cache(sys.argv[1]); S = dscsym.load(None)
p, f, h, maps = c.main
io, ic = struct.unpack_from('<II', h, 0x1c0); f.seek(io); imgs=[]
for _ in range(ic):
    a, mt, ino, po, _ = struct.unpack('<QQQII', f.read(32)); imgs.append((a, po))
CL = [('fp16_vec_3same', 0x9F60C400, 0x0E400400), ('fp16_vec_2misc', 0x9F7E0C00, 0x0E780800),
      ('fp16_byelem', 0x9FC00400, 0x0F000000), ('fp16_scalar_3same', 0xFF60C400, 0x5E400400),
      ('fmlal', 0xBF60FC00, 0x0E20EC00), ('fmlal2', 0xBF60FC00, 0x2E20CC00),
      ('fp16_scalar_arith', 0xFF200C00, 0x1EE00800), ('fp16_scalar_1src', 0xFF207C00, 0x1EE04000),
      ('fcvt_h_conv', 0x7F3F0000, 0x1EE20000), ('fp16_fcsel', 0xFF200C00, 0x1EE00C00)]
for a, po in imgs:
    f.seek(po); s = f.read(300); path = s[:s.index(b'\0')].decode()
    if sys.argv[2] not in path: continue
    hdr = c.read_va(a, 0x8000); ncmds = struct.unpack_from('<I', hdr, 16)[0]; off = 32
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', hdr, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', hdr, off + 64)[0]
            for k in range(nsect):
                so = off + 72 + 80 * k
                addr, size = struct.unpack_from('<QQ', hdr, so + 32); flags = struct.unpack_from('<I', hdr, so + 64)[0]
                if not flags & 0x80000000: continue
                w = np.frombuffer(c.read_va(addr, size)[:size // 4 * 4], dtype='<u4')
                for n, m, v in CL:
                    for i in np.nonzero((w & m) == v)[0]:
                        pc = addr + 4 * int(i); img, fn, o = S.lookup(pc)
                        print('%x %08x %-18s %s+%d' % (pc, int(w[i]), n, fn[:100], o))
        off += sz
    break
