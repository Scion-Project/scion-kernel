#!/usr/bin/env python3
# dscsym.py CACHEDIR [ADDR...] : build a symbol index (a pickle) of an iOS split dyld shared cache of your own root, and look addresses up
# in it: (address, name, image) from the images' symbol tables and the cache's .symbols file; reads only the cache you name.
# The index is kept in $DSCSYM_INDEX (default ./dscsym.pickle); it is derived from the cache, so it stays on your machine (not in this
# repository). As a module: load(CACHEDIR or None) -> Sym; Sym.lookup(unslid_addr) -> (image, name, off).
import struct, glob, os, sys, bisect, pickle

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

class Cache:
    def __init__(self, d):
        self.files = []
        for p in sorted(glob.glob(d + '/dyld_shared_cache_arm64e*')):
            if p.endswith(('.map', '.atlas')): continue
            f = open(p, 'rb'); h = f.read(0x200)
            if not h.startswith(b'dyld_v1'): continue
            mo, mc = u32(h, 0x10), u32(h, 0x14)
            f.seek(mo); maps = []
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32)); maps.append((a, s, o))
            self.files.append((p, f, h, maps))
        self.main = [x for x in self.files if x[0].endswith('arm64e')][0]
    def read_va(self, va, n):
        for p, f, h, maps in self.files:
            if p.endswith('.symbols'): continue
            for a, s, o in maps:
                if a <= va < a + s:
                    f.seek(o + va - a); return f.read(n)
        return None

def build(d):
    c = Cache(d)
    p, f, h, maps = c.main
    io, ic = u32(h, 0x1c0), u32(h, 0x1c4)
    f.seek(io); imgs = []
    for _ in range(ic):
        a, mt, ino, po, _ = struct.unpack('<QQQII', f.read(32))
        f.seek(po, 0) if False else None
        imgs.append((a, po))
    names = []
    for a, po in imgs:
        f.seek(po); s = f.read(512); names.append((a, s[:s.index(b'\0')].decode()))
    names.sort()
    syms = []
    # exported/global symbols from each image's LC_SYMTAB
    for a, path in names:
        hdr = c.read_va(a, 0x8000)
        if not hdr: continue
        ncmds = u32(hdr, 16); off = 32; le = None; st = None; text = None
        for _ in range(ncmds):
            cmd, sz = u32(hdr, off), u32(hdr, off + 4)
            if cmd == 0x19:
                seg = hdr[off + 8:off + 24].rstrip(b'\0')
                vm, vs, fo = u64(hdr, off + 24), u64(hdr, off + 32), u64(hdr, off + 40)
                if seg == b'__LINKEDIT': le = (vm, fo)
                if seg == b'__TEXT': text = (vm, vs)
            if cmd == 0x2: st = struct.unpack_from('<IIII', hdr, off + 8)
            off += sz
        if text: syms.append((text[0], '<' + os.path.basename(path) + '>', path))
        if not (le and st): continue
        symoff, nsyms, stroff, strsize = st
        nl = c.read_va(le[0] + symoff - le[1], nsyms * 16) or b''
        strs = c.read_va(le[0] + stroff - le[1], strsize) or b''
        for i in range(len(nl) // 16):
            strx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nl, i * 16)
            if (ty & 0x0e) == 0x0e and val and strx < len(strs):
                e = strs.find(b'\0', strx); syms.append((val, strs[strx:e].decode('utf-8', 'replace'), path))
    # local symbols from .symbols
    sp = [x for x in c.files if x[0].endswith('.symbols')]
    if sp:
        p2, f2, h2, _ = sp[0]
        lo = u64(h2, 0x48)
        f2.seek(lo); info = f2.read(24)
        nlo, nlc, so, ss, eo, ec = struct.unpack('<6I', info)
        f2.seek(lo + nlo); nl = f2.read(nlc * 16)
        f2.seek(lo + so); strs = f2.read(ss)
        for i in range(nlc):
            strx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nl, i * 16)
            if (ty & 0x0e) == 0x0e and val and strx < len(strs):
                e = strs.find(b'\0', strx); syms.append((val, strs[strx:e].decode('utf-8', 'replace'), None))
    syms.sort(key=lambda x: x[0])
    starts = [a for a, _ in names]
    return {'syms': [(a, n) for a, n, _ in syms], 'imgs': names}

class Sym:
    def __init__(self, idx):
        self.syms = idx['syms']; self.addrs = [a for a, _ in self.syms]
        self.imgs = idx['imgs']; self.istarts = [a for a, _ in self.imgs]
    def lookup(self, va):
        i = bisect.bisect_right(self.istarts, va) - 1
        img = os.path.basename(self.imgs[i][1]) if i >= 0 else '?'
        j = bisect.bisect_right(self.addrs, va) - 1
        if j < 0: return img, '?', 0
        return img, self.syms[j][1], va - self.syms[j][0]

def load(d, cachefile=None):
    cachefile = cachefile or os.environ.get('DSCSYM_INDEX', 'dscsym.pickle')
    if os.path.exists(cachefile): return Sym(pickle.load(open(cachefile, 'rb')))
    idx = build(d); pickle.dump(idx, open(cachefile, 'wb')); return Sym(idx)

if __name__ == '__main__':
    s = load(sys.argv[1])
    print(len(s.syms), 'symbols', len(s.imgs), 'images')
    for a in sys.argv[2:]:
        print(a, s.lookup(int(a, 16)))
