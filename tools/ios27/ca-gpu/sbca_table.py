#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""sbca_table.py: sbca's QuartzCore numbers, one table per iOS build, and the checks that each number is what it claims.

sbca (userland/src/sbca.c) hooks QuartzCore at unslid shared-cache addresses and relies on a few struct offsets and sizes.
They differ per iOS build. Each build's numbers live in tools/ios27/builds/<BUILD>-sbca.txt, one per line:

    NAME  VALUE  ANCHOR  how it was verified (free text)

  NAME     a macro sbca.c uses (EMIT below), or an evidence-only name the checks use (EVIDENCE below)
  VALUE    a hex number; FRAMEBUFFER_FORMAT_WAS and EDR_CLIENT_WAS are comma-separated instruction words
  ANCHOR   sym:<symbol>[+0xOFF]: the cache symbol the value must equal (checked with --symbols), or - for none; a ~ in
           the symbol stands for a space (Objective-C method names)

Commands:
  header BUILD             print the C header sbca.c includes for BUILD (userland/src/sbca-qc/<BUILD>.h)
  sync                     write userland/src/sbca-qc/<BUILD>.h for every table (after editing a table)
  check                    exit 1 when a committed header differs from its table (CI, sbca_table_test.py)
  verify BUILD CACHE [--symbols FILE]
                           check every number of BUILD's table against a shared cache (CACHE = .../dyld_shared_cache_arm64e)
                           by the instruction or pointer it must find there; with FILE (`ipsw dyld symaddr --image QuartzCore
                           CACHE` output) also against the symbols. Prints one line per check, exits 1 on any FAIL.
Reads the cache's files only; prints addresses and instruction words, never the cache's bytes otherwise."""
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T27 = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(T27))
BUILDS = os.path.join(T27, 'builds')
HEADERS = os.path.join(ROOT, 'userland', 'src', 'sbca-qc')
GL_H = os.path.join(HERE, 'hakios', 'hakios.gl.h')

# The macros sbca.c's iOS 27 path uses, in the order its 24A446 block had them (the header reproduces that block).
ADDRESSES = ['QC_ACCEL_SERVER_VTABLE', 'QC_RENDERER_SLOT', 'QC_ACCEL_RENDERER', 'QC_GLES_CONTEXT_CTOR', 'QC_MALLOC_ZONE',
             'QC_MALLOC_ZONE_ONCE', 'QC_MALLOC_ZONE_INIT', 'QC_GLES_CONTEXT_VTABLE']
EMIT = ADDRESSES + ['QC_FUNCTION_SUPPORTED_AT', 'QC_FUNCTION_SUPPORTED', 'QC_TEX_FRAGMENT_SHADERS', 'QC_METAL_WAIT_ONCE',
                    'QC_PUSH_SURFACE_FOUND_BR', 'PUSH_SURFACE_CBNZ', 'PUSH_SURFACE_B', 'QC_FRAMEBUFFER_PIXEL_FORMAT',
                    'FRAMEBUFFER_FORMAT_WAS', 'GLES_CONTEXT_SIZE', 'RENDERER_SIZE', 'RENDERER_BUFFER_SIZE',
                    'SERVER_CONTEXT_AT', 'SERVER_RENDERER_AT', 'SERVER_DISPLAY_AT', 'DISPLAY_RENDERER_AT',
                    'CONTEXT_SET_COMPRESSION', 'QC_EDR_CLIENT']
# Names only the checks use: where the number was seen, and the words sbca.c checks itself before it patches.
EVIDENCE = ['QC_CARENDERER_INIT_EAGL', 'QC_PUSH_SURFACE', 'QC_TEXTURE_FUNCTION_CODES', 'EDR_CLIENT_WAS']
WORDS = {'FRAMEBUFFER_FORMAT_WAS': 3, 'EDR_CLIENT_WAS': 2}
FUNCTION_C3S = 20            # sbca.c: the texture function whose shader slot it fills (empty in QuartzCore)
WINDOW = 0x200               # bytes of a function the instruction patterns are looked for in


class TableError(Exception):
    pass


def table_path(build):
    return os.path.join(BUILDS, build + '-sbca.txt')


def parse(path):
    """NAME -> (value, anchor, evidence); value is an int or a tuple of words."""
    out = {}
    for n, line in enumerate(open(path), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        f = line.split(None, 3)
        if len(f) < 3:
            raise TableError('%s:%d: want NAME VALUE ANCHOR [evidence]' % (path, n))
        name, value, anchor = f[0], f[1], f[2]
        if name not in EMIT and name not in EVIDENCE:
            raise TableError('%s:%d: unknown name %s' % (path, n, name))
        if name in out:
            raise TableError('%s:%d: %s twice' % (path, n, name))
        try:
            if name in WORDS:
                v = tuple(int(w, 16) for w in value.split(','))
                if len(v) != WORDS[name]:
                    raise ValueError
            else:
                v = int(value, 16)
        except ValueError:
            raise TableError('%s:%d: bad value %s' % (path, n, value))
        if anchor != '-' and not re.match(r'^sym:[^+\s]+(\+0x[0-9a-f]+)?$', anchor):
            raise TableError('%s:%d: anchor must be - or sym:NAME[+0xOFF]' % (path, n))
        out[name] = (v, anchor, f[3] if len(f) > 3 else '')
    missing = [k for k in EMIT if k not in out]
    if missing:
        raise TableError('%s: missing %s' % (path, ' '.join(missing)))
    return out


def builds():
    return sorted(os.path.basename(p)[:-len('-sbca.txt')] for p in glob.glob(os.path.join(BUILDS, '*-sbca.txt')))


def header(build, t):
    """The C header: the same lines, in the same order, as sbca.c's iOS 27 block had (so 24A446 builds the same code)."""
    v = {k: t[k][0] for k in t}
    L = ['/* Generated by tools/ios27/ca-gpu/sbca_table.py from tools/ios27/builds/%s-sbca.txt: edit that, then sync. */' % build,
         '/* QuartzCore of build %s: unslid shared-cache addresses, offsets and sizes (numbers only; the evidence for each is in */' % build,
         '/* the table). Included by userland/src/sbca.c. */']
    for k in ADDRESSES:
        L.append('#define %-24s 0x%xUL' % (k, v[k]))
    L.append('#define %-24s 0x%x' % ('QC_FUNCTION_SUPPORTED_AT', v['QC_FUNCTION_SUPPORTED_AT']))
    for k in ('QC_FUNCTION_SUPPORTED', 'QC_TEX_FRAGMENT_SHADERS'):
        L.append('#define %-24s 0x%xUL' % (k, v[k]))
    L.append('#define %-24s 0x0UL          /* iPadOS 18 only */' % 'IOS_GET_BULK_RETURN')
    L.append('#define %-24s 0x0UL' % 'IOS_IVAR_IMPL')
    for k in ('QC_METAL_WAIT_ONCE', 'QC_PUSH_SURFACE_FOUND_BR'):
        L.append('#define %-24s 0x%xUL' % (k, v[k]))
    for k in ('PUSH_SURFACE_CBNZ', 'PUSH_SURFACE_B'):
        L.append('#define %-24s 0x%08xu' % (k, v[k]))
    L.append('#define QC_FRAMEBUFFER_PIXEL_FORMAT 0x%xUL' % v['QC_FRAMEBUFFER_PIXEL_FORMAT'])
    L.append('static const uint32_t framebuffer_format_was[3] = { %s };' %
             ', '.join('0x%08xu' % w for w in v['FRAMEBUFFER_FORMAT_WAS']))
    L.append('static const uint32_t framebuffer_format_bgra[3] = { 0x528a4820u, 0x72a848e0u, 0xd65f03c0u };')
    for k in ('GLES_CONTEXT_SIZE', 'RENDERER_SIZE', 'RENDERER_BUFFER_SIZE'):
        L.append('#define %-24s 0x%x' % (k, v[k]))
    L.append('#define %-24s (*(void **)((char *)(s) + 0x%x))' % ('SERVER_CONTEXT(s)', v['SERVER_CONTEXT_AT']))
    L.append('#define %-24s (*(void **)((char *)(s) + 0x%x))' % ('SERVER_RENDERER(s)', v['SERVER_RENDERER_AT']))
    L.append('#define %-24s (*(char **)((char *)(s) + 0x%x))' % ('SERVER_DISPLAY(s)', v['SERVER_DISPLAY_AT']))
    L.append('#define %-24s (*(void **)((d) + 0x%x))' % ('DISPLAY_RENDERER(d)', v['DISPLAY_RENDERER_AT']))
    L.append('#define %-24s 0x%x' % ('CONTEXT_SET_COMPRESSION', v['CONTEXT_SET_COMPRESSION']))
    L.append('#define %-24s 0x%xUL' % ('QC_EDR_CLIENT', v['QC_EDR_CLIENT']))
    return '\n'.join(L) + '\n'


def header_path(build):
    return os.path.join(HEADERS, build + '.h')


# ---- the cache -------------------------------------------------------------------------------------------------------

class Cache:
    """The mappings of a split arm64e shared cache (main file and subcaches), read on demand."""

    def __init__(self, main):
        self.maps = []
        self.base = None
        for p in sorted(glob.glob(main + '*')):
            if p.endswith(('.map', '.atlas', '.symbols')):
                continue
            with open(p, 'rb') as f:
                h = f.read(0xf0)
                if not h.startswith(b'dyld_v1'):
                    continue
                if p == main:                                   # the main header's sharedRegionStart
                    self.base = struct.unpack_from('<Q', h, 0xe0)[0] or None
                mo, mc = struct.unpack_from('<II', h, 0x10)
                f.seek(mo)
                for _ in range(mc):
                    a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                    self.maps.append((a, s, o, p))
        if not self.maps:
            raise TableError('%s: not a dyld shared cache' % main)
        if self.base is None:
            self.base = min(m[0] for m in self.maps)

    def read(self, va, n):
        for a, s, o, p in self.maps:
            if a <= va and va + n <= a + s:
                with open(p, 'rb') as f:
                    f.seek(o + va - a)
                    return f.read(n)
        return None

    def word(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def words(self, va, n):
        b = self.read(va, 4 * n)
        return None if b is None else list(struct.unpack('<%dI' % n, b))

    def pointer(self, va):
        """The target of a rebased pointer (arm64e shared-cache chained format: runtime offset in the low 34 bits)."""
        b = self.read(va, 8)
        if b is None:
            return None
        v = struct.unpack('<Q', b)[0]
        return None if v == 0 else self.base + (v & ((1 << 34) - 1))

    def cstring(self, va, limit=64):
        b = self.read(va, limit)
        return None if b is None else b.split(b'\0', 1)[0].decode('ascii', 'replace')


# ---- instruction shapes (decoded, never printed) ----------------------------------------------------------------------

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def ldst_x(w):
    """LDR/STR Xt, [Xn, #imm] (unsigned offset): ('ldr'|'str', t, n, imm) or None."""
    if w & 0xffc00000 == 0xf9400000:
        op = 'ldr'
    elif w & 0xffc00000 == 0xf9000000:
        op = 'str'
    else:
        return None
    return op, w & 31, (w >> 5) & 31, ((w >> 10) & 0xfff) << 3


def ldr_x_pre(w):
    """LDR Xt, [Xn, #imm]! : (t, n, imm) or None."""
    if w & 0xffe00c00 != 0xf8400c00:
        return None
    return w & 31, (w >> 5) & 31, sext((w >> 12) & 0x1ff, 9)


def movz_w(w):
    """MOVZ Wd, #imm, lsl #(16*hw): (d, value) or None."""
    if w & 0xff800000 != 0x52800000:
        return None
    return w & 31, ((w >> 5) & 0xffff) << (16 * ((w >> 21) & 3))


def bl_target(w, pc):
    if w & 0xfc000000 != 0x94000000:
        return None
    return pc + 4 * sext(w & 0x3ffffff, 26)


def cbnz_x_offset(w):
    if w & 0xff000000 != 0xb5000000:
        return None
    return 4 * sext((w >> 5) & 0x7ffff, 19)


def b_offset(w):
    if w & 0xfc000000 != 0x14000000:
        return None
    return 4 * sext(w & 0x3ffffff, 26)


# ---- the checks ----------------------------------------------------------------------------------------------------

def read_symbols(path):
    """`ipsw dyld symaddr --image QuartzCore CACHE` output: NAME -> address (the first of each name)."""
    syms = {}
    for line in open(path, errors='replace'):
        m = re.match(r'^0x([0-9a-f]+):\s+\([^)]*\)\s+(\S.*?)\s+\S+$', line.strip())
        if m and int(m.group(1), 16):
            syms.setdefault(m.group(2), int(m.group(1), 16))
    return syms


def gl_functions(path=GL_H):
    """The texture-function names of hakios.gl.h's iOS 27 block, in order."""
    text = open(path).read()
    m = re.search(r'#if HAKIOS_IOS27\b.*?#define HAKIOS_CA_FUNCTIONS(.*?)#define HAKIOS_CA_FUNCTION_COUNT (\d+)', text, re.S)
    if not m:
        raise TableError('%s: no iOS 27 HAKIOS_CA_FUNCTIONS' % path)
    names = ''.join(re.findall(r'"([^"]*)"', m.group(1))).split()
    if len(names) != int(m.group(2)):
        raise TableError('%s: %d names, count says %s' % (path, len(names), m.group(2)))
    return names


def verify(t, cache, syms=None, functions=None):
    """[(status, what)]: status 'ok', 'FAIL' or 'unverified'."""
    v = {k: t[k][0] for k in t}
    out = []

    def say(cond, what):
        out.append(('ok' if cond else 'FAIL', what))

    # symbols: every anchored value equals its symbol (+ offset)
    for k, (val, anchor, _) in sorted(t.items()):
        if anchor == '-':
            continue
        name, _, off = anchor[4:].partition('+')
        name = name.replace('~', ' ')
        if syms is None:
            out.append(('unverified', '%s: anchor %s (no symbol list)' % (k, anchor)))
        elif name not in syms:
            say(False, '%s: symbol %s not in the symbol list' % (k, name))
        else:
            want = syms[name] + (int(off, 16) if off else 0)
            say(want == val, '%s = %#x: %s%s is %#x' % (k, val, name, '+' + off if off else '', want))
    # the two virtual-table entries sbca replaces hold the functions it expects
    p = cache.pointer(v['QC_RENDERER_SLOT'])
    say(p == v['QC_ACCEL_RENDERER'], 'QC_RENDERER_SLOT holds %s (want QC_ACCEL_RENDERER %#x)' %
        (hex(p) if p else p, v['QC_ACCEL_RENDERER']))
    p = cache.pointer(v['QC_GLES_CONTEXT_VTABLE'] + v['QC_FUNCTION_SUPPORTED_AT'])
    say(p == v['QC_FUNCTION_SUPPORTED'], 'QC_GLES_CONTEXT_VTABLE + QC_FUNCTION_SUPPORTED_AT holds %s (want %#x)' %
        (hex(p) if p else p, v['QC_FUNCTION_SUPPORTED']))
    # the instruction words sbca checks before it patches
    w = cache.word(v['QC_PUSH_SURFACE_FOUND_BR'])
    say(w == v['PUSH_SURFACE_CBNZ'], 'QC_PUSH_SURFACE_FOUND_BR: %s (want PUSH_SURFACE_CBNZ %08x)' %
        ('%08x' % w if w is not None else None, v['PUSH_SURFACE_CBNZ']))
    c, b = cbnz_x_offset(v['PUSH_SURFACE_CBNZ']), b_offset(v['PUSH_SURFACE_B'])
    say(c is not None and c == b, 'PUSH_SURFACE_B branches where PUSH_SURFACE_CBNZ does (%s, %s)' % (c, b))
    if 'QC_PUSH_SURFACE' in v:
        d = v['QC_PUSH_SURFACE_FOUND_BR'] - v['QC_PUSH_SURFACE']
        say(0 <= d < WINDOW, 'QC_PUSH_SURFACE_FOUND_BR is push_surface + %#x' % d)
    ws = cache.words(v['QC_FRAMEBUFFER_PIXEL_FORMAT'], 3)
    say(ws is not None and tuple(ws) == v['FRAMEBUFFER_FORMAT_WAS'], 'QC_FRAMEBUFFER_PIXEL_FORMAT: its first three words '
        'are FRAMEBUFFER_FORMAT_WAS')
    if 'EDR_CLIENT_WAS' in v:
        ws = cache.words(v['QC_EDR_CLIENT'], 2)
        say(ws is not None and tuple(ws) == v['EDR_CLIENT_WAS'], 'QC_EDR_CLIENT: its first two words are EDR_CLIENT_WAS '
            '(the words sbca.c checks)')
        say(v['EDR_CLIENT_WAS'] == (0xd503237f, 0xd10143ff), 'EDR_CLIENT_WAS is what sbca.c checks (d503237f, d10143ff)')
    else:
        out.append(('unverified', 'QC_EDR_CLIENT: no EDR_CLIENT_WAS in the table'))
    # the shader table: the slot sbca fills is empty, and the slot before it is not
    p0, pc3s = cache.pointer(v['QC_TEX_FRAGMENT_SHADERS']), cache.pointer(v['QC_TEX_FRAGMENT_SHADERS'] + 8 * FUNCTION_C3S)
    say(p0 is not None and pc3s is None, 'QC_TEX_FRAGMENT_SHADERS: entry 0 set, entry %d (c3s) empty' % FUNCTION_C3S)
    # AccelServer::renderer(): the server's context, renderer and display, the renderer's size, the context call, and the
    # display's renderer field: ldr xA, [xS, #DISPLAY]; ldr xB, [xS, #RENDERER]; str xB, [xA, #DISPLAY_RENDERER]
    code = cache.words(v['QC_ACCEL_RENDERER'], WINDOW // 4) or []
    dec = [ldst_x(w) for w in code]
    say(any(d and d[0] == 'str' and d[3] == v['SERVER_CONTEXT_AT'] for d in dec),
        'renderer(): stores the context at the server + SERVER_CONTEXT_AT (%#x)' % v['SERVER_CONTEXT_AT'])
    say(any(d and d[0] == 'str' and d[3] == v['SERVER_RENDERER_AT'] for d in dec),
        'renderer(): stores the renderer at the server + SERVER_RENDERER_AT (%#x)' % v['SERVER_RENDERER_AT'])
    say(any(movz_w(w) == (1, v['RENDERER_SIZE']) for w in code),
        'renderer(): allocates RENDERER_SIZE (%#x) bytes (mov w1)' % v['RENDERER_SIZE'])
    say(any((ldr_x_pre(w) or (0, 0, None))[2] == v['CONTEXT_SET_COMPRESSION'] for w in code),
        'renderer(): calls the context\'s virtual entry CONTEXT_SET_COMPRESSION (%#x)' % v['CONTEXT_SET_COMPRESSION'])
    seq = False
    for i in range(len(dec) - 2):
        a, b_, c_ = dec[i], dec[i + 1], dec[i + 2]
        if (a and b_ and c_ and a[0] == 'ldr' and b_[0] == 'ldr' and c_[0] == 'str' and a[2] == b_[2]
                and a[3] == v['SERVER_DISPLAY_AT'] and b_[3] == v['SERVER_RENDERER_AT']
                and c_[1] == b_[1] and c_[2] == a[1] and c_[3] == v['DISPLAY_RENDERER_AT']):
            seq = True
    say(seq, 'renderer(): server->display (+%#x) gets the renderer (+%#x) at DISPLAY_RENDERER_AT (%#x)' %
        (v['SERVER_DISPLAY_AT'], v['SERVER_RENDERER_AT'], v['DISPLAY_RENDERER_AT']))
    # -[CARenderer _initWithEAGLContext:options:]: GLES_CONTEXT_SIZE bytes, then GLESContext(void *) on them
    if 'QC_CARENDERER_INIT_EAGL' in v:
        at = v['QC_CARENDERER_INIT_EAGL']
        code = cache.words(at, WINDOW // 4) or []
        size_then_ctor = False
        for i, w in enumerate(code):
            if movz_w(w) == (2, v['GLES_CONTEXT_SIZE']):
                size_then_ctor = any(bl_target(code[j], at + 4 * j) == v['QC_GLES_CONTEXT_CTOR']
                                     for j in range(i + 1, min(i + 9, len(code))))
                if size_then_ctor:
                    break
        say(size_then_ctor, '_initWithEAGLContext: allocates GLES_CONTEXT_SIZE (%#x), then calls QC_GLES_CONTEXT_CTOR' %
            v['GLES_CONTEXT_SIZE'])
        say(any(movz_w(w) == (0, v['RENDERER_BUFFER_SIZE']) for w in code),
            '_initWithEAGLContext: allocates RENDERER_BUFFER_SIZE (%#x) (mov w0)' % v['RENDERER_BUFFER_SIZE'])
    else:
        out.append(('unverified', 'GLES_CONTEXT_SIZE, RENDERER_BUFFER_SIZE: no QC_CARENDERER_INIT_EAGL in the table'))
    # the texture functions: QuartzCore's codes, in order, against hakios.gl.h
    if 'QC_TEXTURE_FUNCTION_CODES' in v and functions is not None:
        at, got = v['QC_TEXTURE_FUNCTION_CODES'], []
        for i in range(len(functions)):
            p = cache.pointer(at + 8 * i)
            s = cache.cstring(p) if p else None
            got.append(s)
        first = next((i for i, (x, y) in enumerate(zip(got, functions)) if x != y), None)
        say(got == functions, 'TextureFunction_codes: the first %d names %s' % (
            len(functions), 'are hakios.gl.h\'s, in its order' if got == functions else
            'differ from hakios.gl.h (first at %s)' % first))
        # where the table ends: the next symbol of the list; every entry from the last name to it is empty
        nxt = min((a for a in (syms or {}).values() if a > at), default=None)
        if nxt is None:
            out.append(('unverified', 'TextureFunction_codes: its end (no symbol list)'))
        else:
            extra = [cache.pointer(a) for a in range(at + 8 * len(functions), nxt, 8)]
            say((nxt - at) // 8 >= len(functions) and not any(extra),
                'TextureFunction_codes: %d entries to the next symbol, none set after the %dth' %
                ((nxt - at) // 8, len(functions)))
    else:
        out.append(('unverified', 'texture functions: no QC_TEXTURE_FUNCTION_CODES or no hakios.gl.h list'))
    return out


def main(argv):
    if len(argv) >= 3 and argv[1] == 'header':
        sys.stdout.write(header(argv[2], parse(table_path(argv[2]))))
        return 0
    if len(argv) == 2 and argv[1] in ('sync', 'check'):
        bad = 0
        for b in builds():
            text = header(b, parse(table_path(b)))
            p = header_path(b)
            if argv[1] == 'sync':
                os.makedirs(HEADERS, exist_ok=True)
                with open(p, 'w') as f:
                    f.write(text)
                print('wrote %s' % os.path.relpath(p, ROOT))
            elif not os.path.exists(p) or open(p).read() != text:
                print('%s differs from its table: run tools/ios27/ca-gpu/sbca_table.py sync' % os.path.relpath(p, ROOT))
                bad = 1
        return bad
    if len(argv) >= 4 and argv[1] == 'verify':
        syms = None
        if '--symbols' in argv:
            syms = read_symbols(argv[argv.index('--symbols') + 1])
        res = verify(parse(table_path(argv[2])), Cache(argv[3]), syms, gl_functions())
        for st, what in res:
            print('%-10s %s' % (st, what))
        n = {s: sum(1 for x, _ in res if x == s) for s in ('ok', 'FAIL', 'unverified')}
        print('%s: %d ok, %d FAIL, %d unverified' % (argv[2], n['ok'], n['FAIL'], n['unverified']))
        return 1 if n['FAIL'] else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
