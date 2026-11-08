#!/usr/bin/env python3
"""Count instructions in an arm64e dyld shared cache that cores without
ARMv8.3 (the S9: A55 v8.2, Mongoose M3 v8.0) cannot execute.

usage: dsc-isa-census.py DIR/dyld_shared_cache_arm64e [IMAGE-SUBSTRING ...]
Scans only S_ATTR_PURE_INSTRUCTIONS sections of each image, so string and
constant data in __TEXT does not count.

Each class says which S9 core lacks it (A55 = Cortex-A55 v8.2 with LSE, RCpc,
RDM, DotProd, FP16, DPB; M3 = Mongoose M3 v8.0) and what handles it today:
  rewrite = tools/a64e/a64e-rewrite.py (ahead of time, 1:1)
  trap    = the 12377 kernel's sweet_a64e_emulate (0319; DIT + LRCPC2: 0350; FHM: 0351; SHA3/SHA512/SB: 0352) / M3 LSE emulation (0342-0344)
  none    = an undefined-instruction crash if it runs (many sit behind a runtime
            CPU-feature check, which the kernel answers for the A55: a static count
            is an upper bound on what runs)
  nop     = a hint, already a NOP on both cores"""
import glob, os, struct, sys
import numpy as np

main = sys.argv[1]
want = sys.argv[2:]
maps = []                                  # (addr, size, path, fileoff)
for p in sorted(glob.glob(main + '*')):
    with open(p, 'rb') as f:
        h = f.read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', h, 0x10)
    with open(p, 'rb') as f:
        f.seek(mo)
        for i in range(mc):
            a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
            maps.append((a, s, p, o))
fhs = {}
def read(addr, n):
    for a, s, p, o in maps:
        if a <= addr < a + s:
            fh = fhs.get(p) or fhs.setdefault(p, open(p, 'rb'))
            fh.seek(o + addr - a)
            return fh.read(n)
    return None

with open(main, 'rb') as f:
    h = f.read(0x200)
io, ic = struct.unpack_from('<II', h, 0x1c0)   # imagesOffset/Count (newer caches)
if not ic:
    io, ic = struct.unpack_from('<II', h, 0x18)
with open(main, 'rb') as f:
    f.seek(io); imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
    paths = []
    for im in imgs:
        f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())

# name: (mask, value).  Order matters: first match wins.
PATS = [
 ('RETAA/RETAB',        0xFFFFFBFF, 0xD65F0BFF, 'both', 'rewrite'),
 ('BR/BLR auth',        0xFE9FF800, 0xD61F0800, 'both', 'rewrite'),
 ('LDRAA/LDRAB',        0xFF200400, 0xF8200400, 'both', 'rewrite (wb: trap)'),
 ('PAC*/AUT*/XPAC reg', 0xFFFF8000, 0xDAC10000, 'both', 'rewrite'),
 ('PACGA',              0xFFE0FC00, 0x9AC03000, 'both', 'trap'),
 ('ERETAA/B',           0xFFFFFBFF, 0xD69F0BFF, 'both', 'none'),
 ('FJCVTZS (JSCVT)',    0xFFFFFC00, 0x1E7E0000, 'both', 'trap'),
 ('FCMLA vector',       0xBF20E400, 0x2E00C400, 'both', 'none'),
 ('FCMLA by element',   0xBF009400, 0x2F001000, 'both', 'none'),
 ('FCADD',              0xBF20EC00, 0x2E00E400, 'both', 'trap'),
 ('LDAPR (RCpc)',       0x3FFFFC00, 0x38BFC000, 'M3',   'rewrite (-> LDAR)'),
 ('LDAPUR/STLUR v8.4',  0x3F000C00, 0x19000000, 'both', 'trap (0350)'),
 ('MOPS CPY/SET v8.8',  0x3B200C00, 0x19000400, 'both', 'none'),
 ('LSE CAS',            0x3FA07C00, 0x08A07C00, 'M3',   'trap (M3)'),
 ('LSE CASP',           0xBFA07C00, 0x08207C00, 'M3',   'trap (M3)'),
 ('LSE LD<op>/SWP',     0x3F200C00, 0x38200000, 'M3',   'trap (M3)'),
 ('DotProd',            0xBFE0FC00, 0x0E809400, 'M3',   'none (M3)'),
 ('DotProd U',          0xBFE0FC00, 0x2E809400, 'M3',   'none (M3)'),
 ('DotProd idx',        0x9FC0F400, 0x0F80E000, 'M3',   'none (M3)'),
 ('SQRDMLAH/SH (RDM)',  0xBF20F400, 0x2E008400, 'M3',   'none (M3)'),
 # ARMv8.4+ classes (neither core); first match wins, so specific encodings first
 ('SB',                 0xFFFFFFFF, 0xD50330FF, 'both', 'trap (0352)'),
 ('CFINV (FlagM)',      0xFFFFFFFF, 0xD500401F, 'both', 'none'),
 ('AXFLAG/XAFLAG',      0xFFFFFFDF, 0xD500405F, 'both', 'none'),
 ('RMIF (FlagM)',       0xFFE07C10, 0xBA000400, 'both', 'none'),
 ('SETF8/16 (FlagM)',   0xFFFFBC1F, 0x3A00080D, 'both', 'none'),
 # MSR <pstatefield>, #imm: op1=3 (bits 18:16), op2 bits 7:5: DIT 010, SSBS 001, SVCR (SMSTART/SMSTOP) 011
 ('MSR DIT imm',        0xFFFFF0FF, 0xD503405F, 'both', 'trap (0350)'),
 ('MSR SSBS imm',       0xFFFFF0FF, 0xD503403F, 'both', 'none'),
 ('SMSTART/SMSTOP (SME)',0xFFFFF0FF, 0xD503407F, 'both', 'none'),
 ('MRS/MSR DIT reg',    0xFFDFFFE0, 0xD51B42A0, 'both', 'trap (0350)'),
 ('MRS/MSR SSBS reg',   0xFFDFFFE0, 0xD51B42C0, 'both', 'none'),
 ('MRS/MSR SVCR (SME)', 0xFFDFFFE0, 0xD51B4240, 'both', 'none'),
 ('MRS/MSR TPIDR2 (SME)',0xFFDFFFE0, 0xD51BD0A0, 'both', 'none'),
 ('MRS CNTxCTSS v8.6',  0xFFFFFFC0, 0xD53BE080, 'both', 'none'),
 ('MRS/MSR S3_x_C15',   0xFFD8F000, 0xD518F000, 'both', 'none (IMPDEF)'),
 ('DC CVADP (DPB2)',    0xFFFFFFE0, 0xD50B7D20, 'both', 'none'),
 ('WFET/WFIT',          0xFFFFFFC0, 0xD5031000, 'both', 'none'),
 ('FRINT32/64 scalar',  0xFF3E7C00, 0x1E284000, 'both', 'none'),
 ('FRINT32/64 vector',  0x9FBFEC00, 0x0E21E800, 'both', 'none'),
 ('BFCVT',              0xFFFFFC00, 0x1E634000, 'both', 'none'),
 ('BFCVTN',             0xBFFFFC00, 0x0EA16800, 'both', 'none'),
 ('BFDOT/BFMLAL',       0xBF20FC00, 0x2E00FC00, 'both', 'none'),
 ('BFDOT/BFMLAL idx',   0xBF00F400, 0x0F00F000, 'both', 'none'),
 ('BFMMLA',             0xFFE0FC00, 0x6E40EC00, 'both', 'none'),
 ('SMMLA/UMMLA/USMMLA', 0xDFE0F400, 0x4E80A400, 'both', 'none'),
 ('USDOT',              0xBFE0FC00, 0x0E809C00, 'both', 'none'),
 ('EOR3 (SHA3)',        0xFFE08000, 0xCE000000, 'both', 'trap (0352)'),
 ('BCAX (SHA3)',        0xFFE08000, 0xCE200000, 'both', 'trap (0352)'),
 ('RAX1 (SHA3)',        0xFFE0FC00, 0xCE608C00, 'both', 'trap (0352)'),
 ('XAR (SHA3)',         0xFFE00000, 0xCE800000, 'both', 'trap (0352)'),
 ('SHA512H/H2/SU1',     0xFFE0F000, 0xCE608000, 'both', 'trap (0352)'),
 ('SHA512SU0',          0xFFFFFC00, 0xCEC08000, 'both', 'trap (0352)'),
 # FHM (kernel agent, docs/MACOS.md 11.2): the old 0x9F60DC00/0x0E20CC00 also caught FMLA/FMLS .2S/.4S and FACGE/FACGT
 ('FMLAL/FMLSL vector', 0xBF60FC00, 0x0E20EC00, 'both', 'trap (0351)'),
 ('FMLAL2/FMLSL2 vector',0xBF60FC00, 0x2E20CC00, 'both', 'trap (0351)'),
 ('FMLAL/FMLSL elem',   0xBFC0B400, 0x0F800000, 'both', 'trap (0351)'),
 ('FMLAL2/FMLSL2 elem', 0xBFC0B400, 0x2F808000, 'both', 'trap (0351)'),
 ('SVE space',          0x1E000000, 0x04000000, 'both', 'none'),
 ('SME space',          0x9E000000, 0x80000000, 'both', 'none'),
 ('CSSC CTZ/CNT v8.9',  0x7FFFF800, 0x5AC01800, 'both', 'none'),
 ('CSSC ABS v8.9',      0x7FFFFC00, 0x5AC02000, 'both', 'none'),
 ('CSSC MIN/MAX reg',   0x7FE0F000, 0x1AC06000, 'both', 'none'),
 ('CSSC MIN/MAX imm',   0x7FC00000, 0x11C00000, 'both', 'none'),
 ('AMX (Apple)',        0xFFFFFC00, 0x00201000, 'both', 'none (Apple only)'),
 ('FP16 scalar (+FCVT H)',0xFFE00000, 0x1EE00000, 'M3',  'none (M3)'),
 ('hint PAC (NOP here)',0xFFFFFD1F, 0xD503211F, 'hint', 'nop'),
 ('hint BTI (NOP here)',0xFFFFFF3F, 0xD503241F, 'hint', 'nop'),
]
masks = np.array([p[1] for p in PATS], dtype=np.uint32)
vals = np.array([p[2] for p in PATS], dtype=np.uint32)
tot = np.zeros(len(PATS), dtype=np.int64); words = 0; per = []; unh = []
UNH = np.array([p[3] == 'both' and p[4].startswith('none') for p in PATS])
for im, path in zip(imgs, paths):
    if want and not any(w in path for w in want):
        continue
    mh = read(im[0], 32)
    if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
        continue
    ncmds, sz = struct.unpack_from('<II', mh, 16)
    lc = read(im[0] + 32, sz); off = 0; cnt = np.zeros(len(PATS), dtype=np.int64); n = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                addr, size = struct.unpack_from('<QQ', lc, so + 32)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if flags & 0x80000000 and size:
                    b = read(addr, size)
                    if not b: continue
                    w = np.frombuffer(b[:len(b) & ~3], dtype=np.uint32)
                    n += w.size; left = np.ones(w.size, bool)
                    for i in range(len(PATS)):
                        hit = left & ((w & masks[i]) == vals[i])
                        cnt[i] += hit.sum(); left &= ~hit
        off += csz
    tot += cnt; words += n; per.append((cnt[:6].sum(), n, path)); unh.append((cnt[UNH].sum(), n, path))
print(f'images {len(per)}  instructions {words:,}  ({words*4/2**20:.0f} MiB of code)')
print(f'  {"class":22s} {"sites":>12s}  {"per 1000":>8s}  {"lacks":5s}  handled by')
for (nm, _, _, who, how), c in zip(PATS, tot):
    print(f'  {nm:22s} {c:12,d}  {c/words*1e3:8.3f}  {who:5s}  {how}')
open_a55 = sum(c for p, c in zip(PATS, tot) if p[3] == 'both' and p[4].startswith('none'))
print(f'unhandled on the A55 (classes marked none, lacks both): {open_a55:,} sites')
print('top images by non-hint PAC ops:')
for c, n, p in sorted(per, reverse=True)[:8]:
    print(f'  {c:9,d} / {n:11,d}  {p}')
print('top images by unhandled A55 sites:')
for c, n, p in sorted(unh, reverse=True)[:12]:
    if c:
        print(f'  {c:9,d} / {n:11,d}  {p}')
