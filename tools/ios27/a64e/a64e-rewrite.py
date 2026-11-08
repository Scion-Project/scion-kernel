#!/usr/bin/env python3
"""Rewrite arm64e code in place so cores without ARMv8.3 (the S9) run it natively.

One instruction for one, in S_ATTR_PURE_INSTRUCTIONS sections only:
  RETAA/RETAB                 -> RET
  BRAA/BRAB/BRAAZ/BRABZ Xn    -> BR Xn
  BLRAA/BLRAB/BLRAAZ/BLRABZ Xn-> BLR Xn
  PAC*/AUT*/XPAC* (register)  -> NOP   (pointers are never signed here)
  LDRAA/LDRAB, no writeback   -> LDR (offset 0..32760) or LDUR (-256..-8)
  LDAPR{B,H,,} (LRCPC)        -> LDAR{B,H,,}   (stronger ordering; the Mongoose M3 lacks LRCPC)
Left for the kernel (sweet_a64e.c) to emulate on trap: PACGA, FJCVTZS, FCADD,
LDRA* with writeback or out-of-range offsets.

  a64e-rewrite.py cache DIR/dyld_shared_cache_arm64e [--only SUBSTR ...] [--dry-run]
  a64e-rewrite.py macho FILE ... [--dry-run]       (thin or fat; arm64e slices)
Files are modified in place (clone them first: cp -c)."""
import argparse, glob, os, struct, sys
import numpy as np

NOP = 0xD503201F
RET = 0xD65F03C0


def rewrite_words(w):
    """w: uint32 array (writable view). Returns a dict of counts."""
    c = {}
    m = (w & 0xFFFFFBFF) == 0xD65F0BFF                       # RETAA/RETAB
    w[m] = RET; c['ret'] = int(m.sum())
    m = (w & 0xFE9FF800) == 0xD61F0800                       # BRA*/BLRA* (+ RETA* already gone)
    op = (w >> 21) & 3
    rn = (w >> 5) & 31
    br = m & (op == 0); blr = m & (op == 1)
    w[br] = (0xD61F0000 | (rn[br] << 5)).astype(np.uint32)
    w[blr] = (0xD63F0000 | (rn[blr] << 5)).astype(np.uint32)
    c['br'] = int(br.sum()); c['blr'] = int(blr.sum())
    m = ((w & 0xFFFF8000) == 0xDAC10000) & (((w >> 10) & 0x3f) <= 17)   # PAC/AUT/XPAC reg forms
    w[m] = NOP; c['pac'] = int(m.sum())
    m = (w & 0xFF200400) == 0xF8200400                       # LDRAA/LDRAB
    wb = (w >> 11) & 1
    imm = (((w >> 12) & 0x1ff) | (((w >> 22) & 1) << 9)).astype(np.int64)
    imm = np.where(imm & 0x200, imm - 0x400, imm) * 8
    rt = w & 31; rn = (w >> 5) & 31
    pos = m & (wb == 0) & (imm >= 0)
    neg = m & (wb == 0) & (imm < 0) & (imm >= -256)
    w[pos] = (0xF9400000 | ((imm[pos] // 8).astype(np.uint32) << 10) | (rn[pos] << 5) | rt[pos]).astype(np.uint32)
    w[neg] = (0xF8400000 | ((imm[neg] & 0x1ff).astype(np.uint32) << 12) | (rn[neg] << 5) | rt[neg]).astype(np.uint32)
    c['ldra'] = int(pos.sum() + neg.sum()); c['ldra_left'] = int(m.sum() - pos.sum() - neg.sum())
    m = (w & 0x3FFFFC00) == 0x38BFC000                       # LDAPR{B,H,,} Rt, [Xn]
    w[m] = ((w[m] & 0xC00003FF) | 0x08DFFC00).astype(np.uint32)
    c['ldapr'] = int(m.sum())
    c['pacga_left'] = int(((w & 0xFFE0FC00) == 0x9AC03000).sum())
    c['fjcvtzs_left'] = int(((w & 0xFFFFFC00) == 0x1E7E0000).sum())
    c['fcadd_left'] = int(((w & 0xBF20EC00) == 0x2E00E400).sum())
    return c


def add(t, c):
    for k, v in c.items():
        t[k] = t.get(k, 0) + v


def text_sections(lc, ncmds):
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                addr, size, foff = struct.unpack_from('<QQI', lc, so + 32)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if flags & 0x80000000 and size:
                    yield addr, size, foff
        off += csz


def do_cache(main, only, dry, skip=()):
    maps = []
    files = {}
    for p in sorted(glob.glob(main + '*')):
        if p.endswith(('.map', '.atlas')):
            continue
        with open(p, 'rb') as f:
            h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        mo, mc = struct.unpack_from('<II', h, 0x10)
        with open(p, 'rb') as f:
            f.seek(mo)
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                maps.append((a, s, p, o))
        files[p] = np.memmap(p, dtype=np.uint8, mode='r' if dry else 'r+')

    def loc(addr):
        for a, s, p, o in maps:
            if a <= addr < a + s:
                return p, o + addr - a
        return None, None

    def read(addr, n):
        p, o = loc(addr)
        return None if p is None else bytes(files[p][o:o + n])

    h = open(main, 'rb').read(0x200)
    io, ic = struct.unpack_from('<II', h, 0x1c0)
    if not ic:
        io, ic = struct.unpack_from('<II', h, 0x18)
    with open(main, 'rb') as f:
        f.seek(io)
        imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
        paths = []
        for im in imgs:
            f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())
    tot, nimg, dirty = {}, 0, 0
    for im, path in zip(imgs, paths):
        if only and not any(o in path for o in only):
            continue
        if any(k in path for k in skip):
            continue
        mh = read(im[0], 32)
        if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
            continue
        ncmds, sz = struct.unpack_from('<II', mh, 16)
        lc = read(im[0] + 32, sz)
        for addr, size, _ in text_sections(lc, ncmds):
            p, o = loc(addr)
            if p is None:
                continue
            mm = files[p]
            buf = np.array(mm[o:o + (size & ~3)]).view(np.uint32) if dry else mm[o:o + (size & ~3)].view(np.uint32)
            add(tot, rewrite_words(buf))
            dirty += size
        nimg += 1
        if not dry and dirty > 256 << 20:   # write back as it goes: a slow (USB) image would pile up dirty pages
            for mm in files.values():
                mm.flush()
            dirty = 0
    if not dry:
        for mm in files.values():
            mm.flush()
    return nimg, tot


def do_macho(path, dry):
    data = np.memmap(path, dtype=np.uint8, mode='r' if dry else 'r+')
    raw = bytes(data[:4096])
    magic = struct.unpack_from('>I', raw)[0]
    slices = []
    if magic in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', raw, 4)[0]
        for i in range(n):
            if magic == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', raw, 8 + i * 20)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', raw, 8 + i * 32)
            if ct == 0x0100000c and (cs & 0xff) == 2:
                slices.append(off)
    elif struct.unpack_from('<I', raw)[0] == 0xfeedfacf:
        if struct.unpack_from('<i', raw, 4)[0] == 0x0100000c and (struct.unpack_from('<i', raw, 8)[0] & 0xff) == 2:
            slices.append(0)
    tot = {}
    for base in slices:
        hdr = bytes(data[base:base + 32])
        ncmds, sz = struct.unpack_from('<II', hdr, 16)
        lc = bytes(data[base + 32:base + 32 + sz])
        for _, size, foff in text_sections(lc, ncmds):
            o = base + foff
            buf = np.array(data[o:o + (size & ~3)]).view(np.uint32) if dry else data[o:o + (size & ~3)].view(np.uint32)
            add(tot, rewrite_words(buf))
    if not dry:
        data.flush()
    return len(slices), tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind', choices=['cache', 'macho'])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--skip', nargs='*', default=[],
                    help='leave images whose path contains one of these untranslated '
                         '(libcorecrypto: its FIPS self-test MACs its own code)')
    a = ap.parse_args()
    if a.kind == 'cache':
        n, tot = do_cache(a.paths[0], a.only, a.dry_run, a.skip)
        print(f'{n} images', tot)
    else:
        for p in a.paths:
            n, tot = do_macho(p, a.dry_run)
            print(f'{p}: {n} arm64e slice(s)', tot)


if __name__ == '__main__':
    main()
