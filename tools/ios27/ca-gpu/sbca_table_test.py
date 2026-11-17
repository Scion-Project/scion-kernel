#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""sbca_table_test.py: sbca's per-build QuartzCore tables (tools/ios27/builds/<BUILD>-sbca.txt) and their checker, without
Apple's files.

  - every table parses and has every number sbca.c needs; the committed headers (userland/src/sbca-qc/) are the tables';
  - 24A446's header gives exactly the numbers sbca.c carried before the tables (the line that runs on phones);
  - `verify` on a made-up cache: a cache laid out the way the table says passes every check, and one wrong offset, word or
    pointer is a FAIL. The made-up cache holds only what this test writes (ARM instruction encodings and pointers), nothing
    of Apple's.
usage: tools/ios27/ca-gpu/sbca_table_test.py"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sbca_table as T  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(('ok    ' if cond else 'FAIL  ') + what)
    fails += not cond


# The numbers sbca.c's SBCA_IOS27 block had before the tables (git: userland/src/sbca.c before tools/ios27/builds/*-sbca.txt).
BEFORE_24A446 = {
    'QC_ACCEL_SERVER_VTABLE': 0x1e8fd77d8, 'QC_RENDERER_SLOT': 0x1e8fd79b8, 'QC_ACCEL_RENDERER': 0x184415838,
    'QC_GLES_CONTEXT_CTOR': 0x18466da0c, 'QC_MALLOC_ZONE': 0x1e709e650, 'QC_MALLOC_ZONE_ONCE': 0x1e709cf68,
    'QC_MALLOC_ZONE_INIT': 0x1844b62b8, 'QC_GLES_CONTEXT_VTABLE': 0x1e8fd8250, 'QC_FUNCTION_SUPPORTED_AT': 0x480,
    'QC_FUNCTION_SUPPORTED': 0x18471afec, 'QC_TEX_FRAGMENT_SHADERS': 0x1e05feb80, 'QC_METAL_WAIT_ONCE': 0x1e4062540,
    'QC_PUSH_SURFACE_FOUND_BR': 0x184464104, 'PUSH_SURFACE_CBNZ': 0xb50001e8, 'PUSH_SURFACE_B': 0x1400000f,
    'QC_FRAMEBUFFER_PIXEL_FORMAT': 0x184461838, 'FRAMEBUFFER_FORMAT_WAS': (0xd503237f, 0x6dbc23e9, 0xa90157f6),
    'GLES_CONTEXT_SIZE': 0xba0, 'RENDERER_SIZE': 0x68, 'RENDERER_BUFFER_SIZE': 0x40000, 'SERVER_CONTEXT_AT': 0x628,
    'SERVER_RENDERER_AT': 0x630, 'SERVER_DISPLAY_AT': 0x60, 'DISPLAY_RENDERER_AT': 0x150, 'CONTEXT_SET_COMPRESSION': 0xf0,
    'QC_EDR_CLIENT': 0x184787f88,
}


# ---- instruction encodings the made-up cache is written with --------------------------------------------------------

def ldr(t, n, imm):
    return 0xf9400000 | ((imm >> 3) << 10) | (n << 5) | t


def str_(t, n, imm):
    return 0xf9000000 | ((imm >> 3) << 10) | (n << 5) | t


def movz(d, value):
    hw = 0
    while value >> 16:
        value >>= 16
        hw += 1
    return 0x52800000 | (hw << 21) | (value << 5) | d


def ldr_pre(t, n, imm):
    return 0xf8400c00 | ((imm & 0x1ff) << 12) | (n << 5) | t


def bl(pc, target):
    return 0x94000000 | (((target - pc) >> 2) & 0x3ffffff)


NOP = 0xd503201f


def made_up_cache(d, t, functions, break_=None):
    """A one-file cache whose words are where table t says; break_ = the name of one thing to get wrong."""
    v = {k: t[k][0] for k in t}
    base = 0x180000000
    pages = {}

    def put(va, data):
        while data:
            page = va & ~0xfff
            buf = pages.setdefault(page, bytearray(0x1000))
            n = min(len(data), page + 0x1000 - va)
            buf[va - page:va - page + n] = data[:n]
            va, data = va + n, data[n:]

    def word(va, w):
        put(va, struct.pack('<I', w))

    def ptr(va, target, auth=False):
        put(va, struct.pack('<Q', (target - base) | (1 << 63 if auth else 0)))

    ptr(v['QC_RENDERER_SLOT'], v['QC_ACCEL_RENDERER'] + (4 if break_ == 'slot' else 0), auth=True)
    ptr(v['QC_GLES_CONTEXT_VTABLE'] + v['QC_FUNCTION_SUPPORTED_AT'], v['QC_FUNCTION_SUPPORTED'], auth=True)
    word(v['QC_PUSH_SURFACE_FOUND_BR'], v['PUSH_SURFACE_CBNZ'])
    for i, w in enumerate(v['FRAMEBUFFER_FORMAT_WAS']):
        word(v['QC_FRAMEBUFFER_PIXEL_FORMAT'] + 4 * i, w)
    for i, w in enumerate(v['EDR_CLIENT_WAS']):
        word(v['QC_EDR_CLIENT'] + 4 * i, w ^ (1 if break_ == 'edr' else 0))
    ptr(v['QC_TEX_FRAGMENT_SHADERS'], base + 0x10)
    # renderer(): x19 = server, x20 = display
    disp = v['DISPLAY_RENDERER_AT'] + (8 if break_ == 'display' else 0)
    code = [NOP, str_(0, 19, v['SERVER_CONTEXT_AT']), movz(1, v['RENDERER_SIZE']), str_(20, 19, v['SERVER_RENDERER_AT']),
            ldr_pre(9, 16, v['CONTEXT_SET_COMPRESSION']), ldr(8, 19, v['SERVER_DISPLAY_AT']),
            ldr(0, 19, v['SERVER_RENDERER_AT']), str_(0, 8, disp), 0xd65f03c0]
    for i, w in enumerate(code):
        word(v['QC_ACCEL_RENDERER'] + 4 * i, w)
    # _initWithEAGLContext: mov w2, #size; bl alloc; mov x1, x?; bl ctor; mov w0, #buffer
    at = v['QC_CARENDERER_INIT_EAGL']
    size = v['GLES_CONTEXT_SIZE'] + (0x10 if break_ == 'size' else 0)
    code = [movz(2, size), bl(at + 4, at + 0x1000), NOP, bl(at + 12, v['QC_GLES_CONTEXT_CTOR']),
            movz(0, v['RENDERER_BUFFER_SIZE'])]
    for i, w in enumerate(code):
        word(at + 4 * i, w)
    # the texture-function codes: pointers to three-letter strings
    strings = base + 0x20000
    for i, name in enumerate(functions):
        if break_ == 'functions' and i == 7:
            name = 'zzz'
        put(strings + 4 * i, name.encode() + b'\0')
        ptr(v['QC_TEXTURE_FUNCTION_CODES'] + 8 * i, strings + 4 * i)
    # the file: header, one mapping per page
    order = sorted(pages)
    hdr = bytearray(0x1000)
    hdr[:16] = b'dyld_v1  arm64e\0'
    struct.pack_into('<II', hdr, 0x10, 0x100, len(order))
    struct.pack_into('<QQ', hdr, 0xe0, base, 0x100000000)   # sharedRegionStart, sharedRegionSize
    for i, page in enumerate(order):
        struct.pack_into('<QQQII', hdr, 0x100 + 32 * i, page, 0x1000, 0x1000 * (i + 1), 5, 5)
    path = os.path.join(d, 'dyld_shared_cache_arm64e')
    with open(path, 'wb') as f:
        f.write(hdr)
        for page in order:
            f.write(pages[page])
    syms = {'__ZN2CA3OGL12MetalContext21TextureFunction_codesE': v['QC_TEXTURE_FUNCTION_CODES'],
            'next table': v['QC_TEXTURE_FUNCTION_CODES'] + 8 * (len(functions) + 1)}
    return path, syms


def main():
    tables = {}
    for b in T.builds():
        try:
            tables[b] = T.parse(T.table_path(b))
            check(True, '%s-sbca.txt parses, every name sbca.c needs is there' % b)
        except T.TableError as e:
            check(False, str(e))
    check({'24A446', '24B156'} <= set(tables), 'tables for 24A446 and 24B156')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'sbca_table.py'), 'check'], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    check(r.returncode == 0, 'userland/src/sbca-qc/*.h are the tables\' headers' + ('' if r.returncode == 0 else ': ' + r.stdout))
    for b in tables:
        conf = os.path.join(T.BUILDS, b + '.conf')
        check(os.path.exists(conf), '%s-sbca.txt belongs to a build with a .conf' % b)
    if '24A446' in tables:
        got = {k: tables['24A446'][k][0] for k in BEFORE_24A446}
        check(got == BEFORE_24A446, '24A446: the numbers sbca.c had before the tables, unchanged')
        h = T.header('24A446', tables['24A446'])
        check('#define DISPLAY_RENDERER(d)      (*(void **)((d) + 0x150))' in h and
              '#define GLES_CONTEXT_SIZE        0xba0' in h, '24A446: its header writes the macros as sbca.c had them')
    functions = T.gl_functions()
    check(len(functions) == 136 and functions[20] == 'c3s', 'hakios.gl.h: 136 iOS 27 names, c3s at %d' % T.FUNCTION_C3S)
    for b in sorted(tables):
        t = tables[b]
        missing = [k for k in T.EVIDENCE if k not in t]
        check(not missing, '%s: has the evidence names (%s)' % (b, ' '.join(missing) or 'all'))
        with tempfile.TemporaryDirectory() as d:
            path, syms = made_up_cache(d, t, functions)
            res = T.verify(t, T.Cache(path), None, functions)
            bad = [w for s, w in res if s == 'FAIL']
            check(not bad, '%s: a made-up cache laid out as the table says passes (%s)' % (b, '; '.join(bad) or
                  '%d ok' % sum(1 for s, _ in res if s == 'ok')))
            res = T.verify({k: t[k] for k in t if k == 'QC_TEXTURE_FUNCTION_CODES' or k in T.EMIT},
                           T.Cache(path), syms, functions)
            check(any(s == 'ok' and 'none set after' in w for s, w in res), '%s: the texture table ends where the next '
                  'symbol starts' % b)
        for what, needle in (('slot', 'QC_RENDERER_SLOT holds'), ('edr', 'QC_EDR_CLIENT'), ('display', 'DISPLAY_RENDERER_AT'),
                             ('size', 'GLES_CONTEXT_SIZE'), ('functions', 'TextureFunction_codes')):
            with tempfile.TemporaryDirectory() as d:
                path, _ = made_up_cache(d, t, functions, break_=what)
                res = T.verify(t, T.Cache(path), None, functions)
                check(any(s == 'FAIL' and needle in w for s, w in res), '%s: a cache with a wrong %s fails (%s)' %
                      (b, what, needle))
    # anchors against a symbol list: a wrong symbol address is a FAIL, a missing list leaves them unverified
    t = tables.get('24B156')
    if t:
        with tempfile.TemporaryDirectory() as d:
            path, _ = made_up_cache(d, t, functions)
            syms = {'__ZN2CA12WindowServer11AccelServer8rendererEv': t['QC_ACCEL_RENDERER'][0] + 4}
            res = T.verify(t, T.Cache(path), syms, functions)
            check(any(s == 'FAIL' and w.startswith('QC_ACCEL_RENDERER =') for s, w in res), 'a symbol at another address fails')
            res = T.verify(t, T.Cache(path), None, functions)
            check(any(s == 'unverified' and 'no symbol list' in w for s, w in res), 'without a symbol list the anchors are '
                  'unverified, not ok')
    print('%d failed' % fails if fails else 'all ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
