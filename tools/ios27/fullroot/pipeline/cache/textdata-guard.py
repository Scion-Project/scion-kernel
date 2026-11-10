#!/usr/bin/env python3
"""textdata-guard.py - find (and put back) DATA words inside __text that an instruction-rewrite pass changed.

The rewrite passes of the cache pipeline (a64e-rewrite, lse-rewrite, a64e-rewrite2, rewrite-grow) look at every 32-bit
word of the pure-instruction sections and rewrite the ones whose bit pattern is an instruction the target CPU lacks.
Hand-written assembly keeps tables in the same sections (bit masks, polynomial coefficients, hash round constants, DCT
tables, ident strings). A table entry with the bit pattern of such an instruction was rewritten like one: 0x1fffffff, a
bit mask, is also FNMSUB H31,H31,H31,H31; 0x78a5636f, a SHA-256 round constant, is also LDUMAXAH. The entry then holds
a branch opcode and the function that reads the table computes garbage (silently: nothing traps).

This tool compares the stock cache with the rewritten one and reports every changed word that is not provably an
instruction, by two rules that need only "can any A64 decoder decode this word" (--undef: the words llvm-objdump
cannot decode with every feature on, one hex word per line, as oracle.sh writes them; all-zero-high-half words count
as undecodable too):

  rule C  an undecodable word within 8 words before AND within 8 words after the changed word;
  rule D  walking back from the changed word over decodable words, the first other thing met is an undecodable word,
          not the function's start (LC_FUNCTION_STARTS) and not an instruction that ends a block (B, BR, RET, BRK...).
          Code is entered at a function start or after such an instruction; a table entry sits behind table entries.

Words whose stock value is an exact pointer-authentication opcode (RETAA/RETAB, BRAA.., BLRAA.., PAC*/AUT*/XPAC*, the
hint space) are reported only with --pac: a table word is never exactly one of those by accident often enough to matter,
and code that follows vendor-specific instructions the decoder does not know would flood the report.

  textdata-guard.py scan  STOCK_MAIN NEW_MAIN --undef FILE [--pac] > candidates.tsv
  textdata-guard.py apply NEW_CACHE_DIR LIST [--dry-run] [--revert]
  textdata-guard.py check STOCK_MAIN NEW_MAIN --undef FILE --reviewed LIST     exit 1 if scan finds a word not in LIST

scan prints: image, section, address, stock word, rewritten word, rules, context (stock words -4..+4). A human reads
the candidates once per cache build (tiny functions between two literal pools are flagged too and are code), and the
ones that are data go to LIST as "VADDR DAMAGED STOCK note" (plus "VADDR code note" lines for the reviewed code
candidates, so that check stays quiet). apply writes STOCK back; it is idempotent, and it refuses a cache whose words are
neither value. The orphaned thunks stay where they are (unreachable). Addresses and words of a vendor cache are that
vendor's: keep LIST out of public repositories.
"""
import argparse, bisect, glob, os, struct, sys
import numpy as np


class Cache:
    def __init__(self, main):
        self.maps, self.files = [], {}
        for p in sorted(glob.glob(main + '*')):
            if p.endswith(('.map', '.atlas', '.a2s', '.symbols')):
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
                    self.maps.append((a, s, p, o))
            self.files[p] = np.memmap(p, dtype=np.uint8, mode='r')
        self.maps.sort()
        self._starts = [m[0] for m in self.maps]
        h = open(main, 'rb').read(0x200)
        io, ic = struct.unpack_from('<II', h, 0x1c0)
        if not ic:
            io, ic = struct.unpack_from('<II', h, 0x18)
        with open(main, 'rb') as f:
            f.seek(io)
            self.imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
            self.paths = []
            for im in self.imgs:
                f.seek(im[3])
                self.paths.append(f.read(512).split(b'\0')[0].decode())

    def view(self, addr, n):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0:
            a, s, p, o = self.maps[i]
            if addr < a + s:
                return self.files[p][o + addr - a:o + addr - a + n]
        return None

    def images(self):
        for im, path in zip(self.imgs, self.paths):
            mh = self.view(im[0], 32)
            if mh is None or struct.unpack_from('<I', bytes(mh))[0] != 0xfeedfacf:
                continue
            ncmds, sz = struct.unpack_from('<II', bytes(mh), 16)
            yield path, im[0], bytes(self.view(im[0] + 32, sz)), ncmds


def load_commands(lc, ncmds):
    sects, linkedit, fstarts = [], None, None
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            name = lc[off + 8:off + 24].split(b'\0')[0]
            vmaddr, _, fileoff = struct.unpack_from('<QQQ', lc, off + 24)
            if name == b'__LINKEDIT':
                linkedit = (vmaddr, fileoff)
            for s in range(struct.unpack_from('<I', lc, off + 64)[0]):
                so = off + 72 + s * 80
                addr, size = struct.unpack_from('<QQ', lc, so + 32)
                if struct.unpack_from('<I', lc, so + 64)[0] & 0x80000000 and size >= 4:
                    sects.append((lc[so:so + 16].split(b'\0')[0].decode(), addr, size))
        elif cmd == 0x26:
            fstarts = struct.unpack_from('<II', lc, off + 8)
        off += csz
    return sects, linkedit, fstarts


def function_starts(cache, base, linkedit, fstarts):
    if not linkedit or not fstarts or not fstarts[1]:
        return np.zeros(0, np.int64)
    v = cache.view(linkedit[0] + fstarts[0] - linkedit[1], fstarts[1])
    if v is None:
        return np.zeros(0, np.int64)
    buf, out, a, i = bytes(v), [], base, 0
    while i < len(buf):
        d = sh = 0
        while True:
            b = buf[i]
            i += 1
            d |= (b & 0x7f) << sh
            sh += 7
            if not b & 0x80 or i >= len(buf):
                break
        if d == 0:
            break
        a += d
        out.append(a)
    return np.array(out, dtype=np.int64)


def is_pac(w):
    """exact pointer-authentication opcodes (what a64e-rewrite turns into their plain forms)"""
    return ((w == 0xD65F0BFF) | (w == 0xD65F0FFF) | ((w & 0xFEFFF800) == 0xD61F0800) | ((w & 0xFEFFF800) == 0xD63F0800) |
            ((w & 0xFFFFF000) == 0xD5032000) | ((w & 0xFFFFF000) == 0xD5033000) | ((w & 0xFFFFF000) == 0xD5034000) |
            ((w & 0xFFFF0000) == 0xDAC10000))


def block_end(w):
    return (((w & 0xFC000000) == 0x14000000) | ((w & 0xFFFFFC1F) == 0xD61F0000) | ((w & 0xFFFFFC1F) == 0xD65F0000) |
            ((w & 0xFEFFF800) == 0xD61F0800) | ((w & 0xFFFFFBFF) == 0xD65F0BFF) | ((w & 0xFFE0001F) == 0xD4200000) |
            ((w & 0xFFFFFBFF) == 0xD69F0BFF) | (w == 0xD69F03E0))


def scan(stock, new, undef, pac=False):
    S, R = Cache(stock), Cache(new)
    for path, base, lc, ncmds in S.images():
        sects, linkedit, fst = load_commands(lc, ncmds)
        fsa = function_starts(S, base, linkedit, fst)
        for sn, addr, size in sects:
            sv, rv = S.view(addr, size & ~3), R.view(addr, size & ~3)
            if sv is None or rv is None or len(sv) != len(rv):
                continue
            w, r = np.frombuffer(sv, '<u4'), np.frombuffer(rv, '<u4')
            n = len(w)
            d = np.flatnonzero(w != r)
            if not pac and len(d):
                d = d[~is_pac(w[d])]
            if not len(d):
                continue
            inv = np.isin(w, undef) | (w < 0x10000)
            cs = np.concatenate(([0], np.cumsum(inv)))
            c = ((cs[d] - cs[np.maximum(d - 8, 0)]) >= 1) & ((cs[np.minimum(d + 9, n)] - cs[np.minimum(d + 1, n)]) >= 1)
            brk = inv | block_end(w)
            last = np.maximum.accumulate(np.where(brk, np.arange(n, dtype=np.int64), -1))
            j = np.where(d > 0, last[np.maximum(d - 1, 0)], -1)
            fidx = (fsa - addr) // 4
            k = np.searchsorted(fidx, d, side='right') - 1
            fstart = np.maximum(np.where(k >= 0, fidx[np.maximum(k, 0)], 0), 0) if len(fidx) else np.zeros(len(d), np.int64)
            dd = (j >= fstart) & (j >= 0) & inv[np.maximum(j, 0)]
            for i in np.flatnonzero(c | dd):
                x = int(d[i])
                yield (path, sn, addr + 4 * x, int(w[x]), int(r[x]), ('C' if c[i] else '') + ('D' if dd[i] else ''),
                       ' '.join('%08x' % int(y) for y in w[max(0, x - 4):x + 5]))


def read_list(path):
    data, code = [], set()
    for ln in open(path):
        p = ln.split('#')[0].split()
        if len(p) >= 2 and p[1] == 'code':
            code.add(int(p[0], 16))
        elif len(p) >= 3:
            data.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))
    return data, code


def apply(cachedir, lst, dry=False, revert=False):
    maps = []
    for p in sorted(glob.glob(cachedir + '/dyld_shared_cache_arm64e*')):
        if p.endswith(('.map', '.atlas', '.symbols')):
            continue
        with open(p, 'rb') as f:
            h = f.read(0x200)
            if not h.startswith(b'dyld_v1'):
                continue
            mo, mc = struct.unpack_from('<II', h, 0x10)
            f.seek(mo)
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                maps.append((a, s, o, p))
    ents, _ = read_list(lst)
    plan, already, bad = [], 0, []
    for va, damaged, stock in ents:
        hit = [(o + va - a, p) for a, s, o, p in maps if a <= va < a + s]
        if not hit:
            bad.append('%#x: address not mapped' % va)
            continue
        off, p = hit[0]
        with open(p, 'rb') as f:
            f.seek(off)
            cur = struct.unpack('<I', f.read(4))[0]
        want, other = (damaged, stock) if revert else (stock, damaged)
        if cur == want:
            already += 1
        elif cur == other:
            plan.append((p, off, want))
        else:
            bad.append('%#x: holds %08x, neither %08x nor %08x' % (va, cur, damaged, stock))
    if bad:
        print('textdata-guard: NOT APPLIED (another cache build?):\n' + '\n'.join('  ' + b for b in bad[:20]))
        return 1
    if not dry:
        for p, off, want in plan:
            with open(p, 'r+b') as f:
                f.seek(off)
                f.write(struct.pack('<I', want))
    print('textdata-guard: %d words %s, %d already there, %d listed%s' % (
        len(plan), 'damaged again' if revert else 'restored', already, len(ents), ' (dry run)' if dry else ''))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['scan', 'apply', 'check'])
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--undef')
    ap.add_argument('--reviewed')
    ap.add_argument('--pac', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--revert', action='store_true')
    o = ap.parse_args()
    if o.mode == 'apply':
        sys.exit(apply(o.a, o.b, o.dry_run, o.revert))
    undef = np.array(sorted(int(x, 16) for x in open(o.undef).read().split()), dtype=np.uint32)
    known = set()
    if o.mode == 'check':
        data, code = read_list(o.reviewed)
        known = {va for va, _, _ in data} | code
    new = 0
    for path, sn, va, sw, rw, rules, ctx in scan(o.a, o.b, undef, o.pac):
        if o.mode == 'check' and va in known:
            continue
        new += 1
        print('%s\t%s\t%#x\t%08x\t%08x\t%s\t%s' % (path, sn, va, sw, rw, rules, ctx))
    if o.mode == 'check':
        print('textdata-guard: %d candidate(s) not in the reviewed list' % new, file=sys.stderr)
        sys.exit(1 if new else 0)


if __name__ == '__main__':
    main()
