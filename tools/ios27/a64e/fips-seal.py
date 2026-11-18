#!/usr/bin/env python3
"""fips-seal.py - keep corecrypto's FIPS integrity seal true after its code is rewritten (18 November 2026).

corecrypto's power-on self-test (fipspost_post_integrity, run in user space by /usr/libexec/cc_fips_test) computes
HMAC-SHA256, key = 32 zero bytes, over the image's first __TEXT,__text (or __TEXT_EXEC,__text) section and compares it
with the 32 bytes of its __TEXT,__fips_hmacs section (apple/corecrypto: cc_fips/src/fipspost_get_hmac.c,
fipspost_post_integrity.c). Rewriting libcorecrypto's instructions (a64e-rewrite.py, lse-rewrite.py) changes __text, so
the stored value no longer matches and the self-test fails; that is why the rewrite tools skipped the library and why
its RETAB/BLRAA/BRAA words kept trapping into the kernel's emulation (the S9's hottest A64E sites, docs/ios27/CORECRYPTO.md).

This tool recomputes the value from the rewritten code and writes it into __fips_hmacs, but only for an image whose seal
was TRUE in the reference (the stock cache, or the cache before the rewrite): the check stays exactly as strong as
Apple's (any change made after the reseal fails it again), it just covers the rewritten code instead of the original.

  fips-seal.py check   CACHE_MAIN [--ref REF_MAIN]          per image: sealed / unsealed-in-ref / BROKEN (exit 1 if any BROKEN)
  fips-seal.py reseal  CACHE_MAIN --ref REF_MAIN --log FILE [--only SUBSTR ...] [--dry-run]
  fips-seal.py restore CACHE_MAIN --log FILE               put the logged original values back (exit 1 if a value is not the logged new one)
  fips-seal.py ... --macho FILE                            the same for one thin 64-bit Mach-O (tests; CACHE_MAIN/REF_MAIN are files)

An image is BROKEN when its reference seal was true and its current one is not: something changed its __text after (or
without) a reseal. An image whose seal is already false in the reference (libcorecrypto_noasm/_trace in 24B156's cache)
is reported as unsealed-in-ref and never written. Pure Python (no numpy); files are modified in place (clone them first).
"""
import argparse, glob, hashlib, hmac, struct, sys

KEY = bytes(32)
SEAL = 32


def seal_of(text):
    return hmac.new(KEY, text, hashlib.sha256).digest()


class Cache:
    """a dyld shared cache (main file + subcaches) addressed by unslid VA"""

    def __init__(self, main, write=False):
        self.maps, self.fh = [], {}
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
            self.fh[p] = open(p, 'r+b' if write else 'rb')
        h = open(main, 'rb').read(0x200)
        io, ic = struct.unpack_from('<II', h, 0x1c0)
        if not ic:
            io, ic = struct.unpack_from('<II', h, 0x18)
        self.images = []
        with open(main, 'rb') as f:
            f.seek(io)
            imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
            for im in imgs:
                f.seek(im[3])
                self.images.append((f.read(512).split(b'\0')[0].decode(), im[0]))

    def loc(self, addr, n):
        for a, s, p, o in self.maps:
            if a <= addr and addr + n <= a + s:
                return p, o + addr - a
        raise ValueError(f'{addr:#x}+{n:#x} is not inside one mapping')

    def read(self, addr, n):
        p, o = self.loc(addr, n)
        self.fh[p].seek(o)
        return self.fh[p].read(n)

    def write(self, addr, data):
        p, o = self.loc(addr, len(data))
        self.fh[p].seek(o)
        self.fh[p].write(data)

    def close(self):
        for f in self.fh.values():
            f.close()


class Macho:
    """one thin 64-bit Mach-O file addressed by VA (through its segments' file ranges)"""

    def __init__(self, path, write=False):
        self.f = open(path, 'r+b' if write else 'rb')
        hdr = self.f.read(32)
        if struct.unpack_from('<I', hdr)[0] != 0xfeedfacf:
            raise ValueError(f'{path}: not a thin 64-bit Mach-O')
        ncmds, sz = struct.unpack_from('<II', hdr, 16)
        lc = self.f.read(sz)
        self.segs, off, self.base = [], 0, None
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', lc, off)
            if cmd == 0x19:
                va, vs, fo, fs = struct.unpack_from('<QQQQ', lc, off + 24)
                self.segs.append((va, fs, fo))
                if fo == 0 and fs:
                    self.base = va
            off += csz
        self.images = [('(the Mach-O file)', self.base)]   # one image: matched with the reference's one

    def loc(self, addr, n):
        for va, fs, fo in self.segs:
            if va <= addr and addr + n <= va + fs:
                return fo + addr - va
        raise ValueError(f'{addr:#x}+{n:#x} is not inside one segment file range')

    def read(self, addr, n):
        self.f.seek(self.loc(addr, n))
        return self.f.read(n)

    def write(self, addr, data):
        self.f.seek(self.loc(addr, len(data)))
        self.f.write(data)

    def close(self):
        self.f.close()


def fips_sections(img, base):
    """(text_addr, text_size, seal_addr) of the image at base, or None when it has no 32-byte __TEXT,__fips_hmacs.
    The text is the section fipspost_get_hmac hashes: the first __text of the first segment whose name starts with __TEXT."""
    mh = img.read(base, 32)
    if struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
        return None
    ncmds, sz = struct.unpack_from('<II', mh, 16)
    lc = img.read(base + 32, sz)
    off, text, seal = 0, None, None
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            seg = lc[off + 8:off + 24].split(b'\0')[0]
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                sn = lc[so:so + 16].split(b'\0')[0]
                sg = lc[so + 16:so + 32].split(b'\0')[0]
                addr, size = struct.unpack_from('<QQ', lc, so + 32)
                if text is None and seg.startswith(b'__TEXT') and sn == b'__text' and sg in (b'__TEXT', b'__TEXT_EXEC'):
                    text = (addr, size)
                if sg == b'__TEXT' and sn == b'__fips_hmacs' and size == SEAL:
                    seal = addr
        off += csz
    if text is None or seal is None:
        return None
    return text[0], text[1], seal


def sealed_images(img, only=()):
    """{path: (text_addr, text_size, seal_addr, stored, computed)} for every image with a FIPS seal"""
    out = {}
    for path, base in img.images:
        if only and not any(o in path for o in only):
            continue
        try:
            fs = fips_sections(img, base)
        except ValueError:
            continue
        if fs is None:
            continue
        ta, tsz, sa = fs
        out[path] = (ta, tsz, sa, img.read(sa, SEAL), seal_of(img.read(ta, tsz)))
    return out


def status(cur, ref):
    """'sealed' | 'resealable' | 'unsealed-in-ref' | 'BROKEN' for one image (cur, ref: sealed_images entries; ref may be None)"""
    _, _, _, stored, computed = cur
    if stored == computed:
        return 'sealed'
    if ref is None:
        return 'BROKEN'
    rta, rtsz, rsa, rstored, rcomputed = ref
    if rstored != rcomputed:
        return 'unsealed-in-ref'
    if (rta, rtsz, rsa) != cur[:3] or rstored != stored:
        return 'BROKEN'           # layout or stored value changed under us: do not guess
    return 'resealable'


def open_img(path, macho, write=False):
    return Macho(path, write) if macho else Cache(path, write)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=['check', 'reseal', 'restore'])
    ap.add_argument('path')
    ap.add_argument('--ref')
    ap.add_argument('--log')
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--macho', action='store_true', help='PATH and --ref are thin Mach-O files, not caches')
    a = ap.parse_args()

    if a.mode == 'restore':
        if not a.log:
            ap.error('restore needs --log')
        img = open_img(a.path, a.macho, write=not a.dry_run)
        bad = n = 0
        for line in open(a.log):
            if line.startswith('#') or not line.strip():
                continue
            sa, old, new, path = line.split(None, 3)
            sa = int(sa, 16)
            cur = img.read(sa, SEAL).hex()
            if cur == old:
                print(f'restore: {path.strip()}: already the original value')
            elif cur != new:
                print(f'restore: {path.strip()}: {sa:#x} holds {cur}, not the logged new value: left alone')
                bad += 1
                continue
            elif not a.dry_run:
                img.write(sa, bytes.fromhex(old))
            n += 1
        img.close()
        print(f'restore: {n} ok, {bad} bad')
        sys.exit(1 if bad else 0)

    if a.mode == 'reseal' and (not a.ref or not a.log):
        ap.error('reseal needs --ref (the cache before the rewrite) and --log')
    cur_img = open_img(a.path, a.macho, write=(a.mode == 'reseal' and not a.dry_run))
    ref = sealed_images(open_img(a.ref, a.macho), a.only) if a.ref else {}
    cur = sealed_images(cur_img, a.only)
    log = open(a.log, 'a') if a.mode == 'reseal' and not a.dry_run else None
    broken = 0
    for path, e in sorted(cur.items()):
        st = status(e, ref.get(path) if a.ref else None)
        ta, tsz, sa, stored, computed = e
        if a.mode == 'reseal' and st == 'resealable':
            if log:
                log.write(f'{sa:#x} {stored.hex()} {computed.hex()} {path}\n')
                log.flush()
                cur_img.write(sa, computed)
            st = 'resealed' + (' (dry run)' if a.dry_run else '')
        if st == 'BROKEN':
            broken += 1
        if a.mode == 'check' and st == 'resealable':
            st = 'BROKEN (resealable: fips-seal.py reseal)'
            broken += 1
        print(f'{st:16s} {path}  __text {ta:#x}+{tsz:#x} seal {sa:#x} stored {stored.hex()[:16]} computed {computed.hex()[:16]}')
    cur_img.close()
    print(f'{a.mode}: {len(cur)} sealed image(s), {broken} broken')
    sys.exit(1 if broken else 0)


if __name__ == '__main__':
    main()
