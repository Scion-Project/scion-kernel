#!/usr/bin/env python3
"""Move the hottest LSE atomics of the iOS 26 shared cache out of the Mongoose M3's trap path.

The M3 (ARMv8.0) has no LSE: each CAS / LD<op> / SWP it runs traps to the kernel, which emulates it
(xnu 0342). A few sites (malloc, free, objc_retain/release, os_unfair_lock, CFRetain/Release) take
most of the millions of traps a second. This rewrites such a site as a branch to an LL/SC routine
placed in the zero padding after its image's __TEXT, which branches back:

  site:  CAS{A,L,AL}{B,H,,} Rs, Rt, [Rn]      ->  B stub
  stub:  1: LD{A}XR T, [Rn]                       (acquire if the original acquires)
            SUB  S, T, Rs (UXTB/UXTH for B/H)    no flags touched
            CBNZ S, 2f
            ST{L}XR wS, Rt, [Rn]                  (release if the original releases)
            CBNZ wS, 1b
         2: MOV  Rs, T
            B    site + 4
  LD{ADD,CLR,EOR,SET}, SWP likewise (old value to Rt, new one built in a scratch register).
The stub saves its scratch registers (three of x9-x15) below SP lowered by 160 (beyond the
128-byte red zone, so a signal frame cannot land on them) and restores them: every register but
NZCV-free flags is preserved, whatever the calling convention (swift_*_preservemost callers keep
x9-x15 live). Sites already patched (a B) are left alone; --restore LOG puts the original
instructions back from a previous run's output.

  lse-hotpatch.py CACHE_MAIN ADDR... [--dry-run]     (unslid cache addresses, e.g. 0x18ec74bbc)
Needs build/tools/ipsw/ipsw (function bounds, register use) and the .a2s cache of that cache."""
import argparse, glob, os, re, struct, subprocess, sys

IPSW = os.path.join(os.path.dirname(__file__), '../../build/tools/ipsw/ipsw')


def u32(b, o=0):
    return struct.unpack_from('<I', b, o)[0]


class Cache:
    def __init__(self, main, dry):
        self.main, self.dry, self.maps, self.fh = main, dry, [], {}
        for p in sorted(glob.glob(main + '*')):
            if p.endswith(('.map', '.atlas', '.a2s')):
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
        h = open(main, 'rb').read(0x200)
        io, ic = struct.unpack_from('<II', h, 0x1c0)
        if not ic:
            io, ic = struct.unpack_from('<II', h, 0x18)
        with open(main, 'rb') as f:
            f.seek(io)
            self.images = [struct.unpack('<QQQII', f.read(32))[0] for _ in range(ic)]
        self.text = []                          # (start, end of sections, end of segment)
        for base in self.images:
            mh = self.read(base, 32)
            if not mh or u32(mh) != 0xfeedfacf:
                continue
            ncmds, sz = struct.unpack_from('<II', mh, 16)
            lc = self.read(base + 32, sz)
            off = 0
            for _ in range(ncmds):
                cmd, csz = struct.unpack_from('<II', lc, off)
                if cmd == 0x19 and lc[off + 8:off + 14] == b'__TEXT':
                    vmaddr, vmsize = struct.unpack_from('<QQ', lc, off + 24)
                    nsect = u32(lc, off + 64)
                    end = max([vmaddr + 32 + sz] + [struct.unpack_from('<QQ', lc, off + 72 + s * 80 + 32)[0] +
                               struct.unpack_from('<QQ', lc, off + 72 + s * 80 + 32)[1] for s in range(nsect)])
                    self.text.append((vmaddr, end, vmaddr + vmsize))
                off += csz
        self.text.sort()

    def loc(self, addr):
        for a, s, p, o in self.maps:
            if a <= addr < a + s:
                return p, o + addr - a
        raise ValueError(f'{addr:#x} not mapped')

    def read(self, addr, n):
        p, o = self.loc(addr)
        with open(p, 'rb') as f:
            f.seek(o)
            return f.read(n)

    def write(self, addr, data):
        p, o = self.loc(addr)
        if self.dry:
            return
        with open(p, 'r+b') as f:
            f.seek(o)
            f.write(data)

    def slack(self, addr):
        """[start, end) of the zero padding after the __TEXT holding addr, up to the next image."""
        for i, (start, end, segend) in enumerate(self.text):
            if start <= addr < end:
                nxt = self.text[i + 1][0] if i + 1 < len(self.text) else segend
                lim = min(nxt, (segend + 0x3fff) & ~0x3fff)
                s = (end + 15) & ~15
                # skip what earlier runs placed there (non-zero words)
                data = self.read(s, max(0, lim - s))
                used = 0
                for k in range(0, len(data) - 3, 4):
                    if u32(data, k):
                        used = k + 4
                return (s + used + 15) & ~15, lim
        raise ValueError(f'{addr:#x} in no __TEXT')


def function_regs(cache, addr, a2s):
    """Registers the function containing addr mentions (numbers), and whether the listing covers addr."""
    out = subprocess.run([IPSW, 'dyld', 'a2s', cache, hex(addr), '--cache', a2s], capture_output=True, text=True).stdout
    m = re.search(r':\s+(\S+?)(?:\s+\+\s+(\d+))?\s*$', out.strip().splitlines()[-1])
    if not m:
        raise ValueError(f'no symbol for {addr:#x}: {out!r}')
    sym = m.group(1)
    lst = subprocess.run([IPSW, 'dsc', 'disass', cache, '--symbol', sym, '--cache', a2s], capture_output=True,
                         text=True).stdout
    if not re.search(r'0x%x:' % addr, lst):
        start = addr - int(m.group(2) or 0)
        lst = subprocess.run([IPSW, 'dsc', 'disass', cache, '--vaddr', hex(start), '--count', '400', '--cache', a2s],
                             capture_output=True, text=True).stdout
    regs, seen = set(), False
    for line in lst.splitlines():
        mm = re.match(r'\s*0x([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(.*)$', line)
        if not mm:
            continue
        if int(mm.group(1), 16) == addr:
            seen = True
        ops = mm.group(2).split(';')[0]
        for r in re.findall(r'\b[xw](\d+)\b', ops):
            regs.add(int(r))
        if re.search(r'\blr\b', ops):
            regs.add(30)
        if re.search(r'\bfp\b', ops):
            regs.add(29)
    return sym, regs, seen


# ---- encoders ---------------------------------------------------------------------------------------
def ldxr(size, t, n, acq):
    return 0x085F7C00 | size << 30 | (1 << 15 if acq else 0) | n << 5 | t


def stxr(size, s, t, n, rel):
    return 0x08007C00 | size << 30 | (1 << 15 if rel else 0) | s << 16 | n << 5 | t


def alu(op, sf, d, n, m):
    base = {'add': 0x0B000000, 'sub': 0x4B000000, 'bic': 0x0A200000, 'eor': 0x4A000000, 'orr': 0x2A000000}[op]
    return base | sf << 31 | m << 16 | n << 5 | d


def sub_ext(d, n, m, opt):                       # SUB Wd, Wn, Wm, UXTB/UXTH
    return 0x4B200000 | m << 16 | opt << 13 | n << 5 | d


def cbnz(sf, t, off):
    return (0xB5000000 if sf else 0x35000000) | ((off // 4) & 0x7FFFF) << 5 | t


def mov(sf, d, m):
    return (0xAA0003E0 if sf else 0x2A0003E0) | m << 16 | d


def b(off):
    return 0x14000000 | ((off // 4) & 0x3FFFFFF)


def stub_for(w, free):
    """The LL/SC words for LSE instruction w (minus the final B back), or None. With too few dead
    temporaries, x9-x11 are saved below a lowered SP (past the 128-byte red zone) and restored."""
    rn = (w >> 5) & 31
    if rn == 31:
        return None
    # Always save the scratch registers: "unmentioned in the function" is not "free" for special
    # calling conventions (swift_retain_preservemost's callers keep x9/x10 across the call; the
    # first version, which borrowed them, crashed every Swift process).
    rt, rs = w & 31, (w >> 16) & 31
    saved = [r for r in range(9, 16) if r not in (rt, rs, rn)][:3]
    words = stub_body(w, saved)
    if words is None:
        return None
    a, b_, c = saved
    pro = [0xD10283FF, 0xA90003E0 | b_ << 10 | a, 0xF9000BE0 | c]          # SUB SP,#160; STP; STR [SP,#16]
    epi = [0xA94003E0 | b_ << 10 | a, 0xF9400BE0 | c, 0x910283FF]          # LDP; LDR; ADD SP,#160
    return pro + words + epi


def stub_body(w, free):
    size, rt, rn, rs = w >> 30, w & 31, (w >> 5) & 31, (w >> 16) & 31
    sf = 1 if size == 3 else 0
    if (w & 0x3fa07c00) == 0x08a07c00:                        # CAS
        acq, rel = bool(w & (1 << 22)), bool(w & (1 << 15))
        if len(free) < 2 or rs == 31:
            return None
        t, s = free[0], free[1]
        cmpw = sub_ext(s, t, rs, size) if size < 2 else alu('sub', sf, s, t, rs)
        return [ldxr(size, t, rn, acq), cmpw, cbnz(sf, s, 12), stxr(size, s, rt, rn, rel), cbnz(0, s, -16),
                mov(sf, rs, t)]
    if (w & 0x3f200c00) == 0x38200000:                        # LD<op>, SWP
        acq, rel = bool(w & (1 << 23)), bool(w & (1 << 22))
        o3, opc = (w >> 15) & 1, (w >> 12) & 7
        direct = rt not in (31, rs, rn)
        if o3 == 1 and opc == 0:                              # SWP
            need = 1 if direct else 2
            if len(free) < need:
                return None
            t = rt if direct else free[0]
            s = free[0] if direct else free[1]
            words = [ldxr(size, t, rn, acq), stxr(size, s, rs, rn, rel), cbnz(0, s, -8)]
        elif o3 == 0 and opc <= 3:                            # LDADD, LDCLR, LDEOR, LDSET
            need = 2 if direct else 3
            if len(free) < need:
                return None
            t = rt if direct else free[0]
            u, s = (free[0], free[1]) if direct else (free[1], free[2])
            op = ('add', 'bic', 'eor', 'orr')[opc]
            words = [ldxr(size, t, rn, acq), alu(op, sf, u, t, rs), stxr(size, s, u, rn, rel), cbnz(0, s, -12)]
        else:
            return None
        if not direct and rt != 31:
            words.append(mov(sf, rt, t))
        return words
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache')
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--a2s', required=True)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--restore', help="a previous run's output: write each site's original word back")
    a = ap.parse_args()
    c = Cache(a.cache, a.dry_run)
    if a.restore:
        for line in open(a.restore):
            m = re.match(r'(0x[0-9a-f]+): \S+: (0x[0-9a-f]{8}) -> B', line)
            if m:
                site, word = int(m.group(1), 16), int(m.group(2), 16)
                if (u32(c.read(site, 4)) & 0xFC000000) == 0x14000000:
                    c.write(site, struct.pack('<I', word))
                    print(f'{site:#x}: restored {word:#010x}')
        return
    done = 0
    for s in a.addrs:
        addr = int(s, 16)
        w = u32(c.read(addr, 4))
        if (w & 0xFC000000) == 0x14000000:
            print(f'{addr:#x}: already patched')
            continue
        sym, regs, seen = function_regs(a.cache, addr, a.a2s)
        if not seen:
            print(f'{addr:#x}: {sym}: site not in the listing, skipped')
            continue
        free = [r for r in range(9, 16) if r not in regs]
        words = stub_for(w, free)
        if words is None:
            print(f'{addr:#x}: {sym}: {w:#010x} not handled (free {free}), left to the kernel')
            continue
        start, lim = c.slack(addr)
        total = 4 * (len(words) + 1)
        if start + total > lim:
            print(f'{addr:#x}: {sym}: no room after its __TEXT ({lim - start} bytes)')
            continue
        words.append(b(addr + 4 - (start + 4 * len(words))))
        c.write(start, struct.pack(f'<{len(words)}I', *words))
        c.write(addr, struct.pack('<I', b(start - addr)))
        print(f'{addr:#x}: {sym}: {w:#010x} -> B {start:#x} ({len(words)} words, scratch {free[:3]})')
        done += 1
    print(f'{done} sites patched' + (' (dry run)' if a.dry_run else ''))


if __name__ == '__main__':
    main()
