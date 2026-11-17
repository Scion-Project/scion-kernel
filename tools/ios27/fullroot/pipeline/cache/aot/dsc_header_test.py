#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""dsc_header_test.py: dsc_header.py on made-up headers (no Apple file): the shared region, the fixed header's end and
where a header's zero tail begins, for the header shapes iOS 27's caches have (one, two and eight mappings), and the
24A446 numbers rewrite-grow.py had written in. usage: tools/ios27/fullroot/pipeline/cache/aot/dsc_header_test.py"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsc_header as H  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(('ok    ' if cond else 'FAIL  ') + what)
    fails += not cond


# where mapping_and_slide_info[] starts in iOS 27's subcache headers, by mapping count (not right after mapping_info[])
WITH_SLIDE_AT = {1: 0x270, 2: 0x2a8, 8: 0x3f8}


def header(mc, start=0x180000000, size=0x17c228000, mo=0x238):
    h = bytearray(0x4000)
    h[:16] = b'dyld_v1  arm64e\0'
    wo = WITH_SLIDE_AT[mc]
    struct.pack_into('<II', h, 0x10, mo, mc)
    struct.pack_into('<II', h, 0x138, wo, mc)
    struct.pack_into('<QQ', h, 0xe0, start, size)
    for i in range(mc):
        struct.pack_into('<QQQII', h, mo + 32 * i, start + 0x100000 * i, 0x4000, 0x4000 * i, 5, 5)
        struct.pack_into('<QQQQQQII', h, wo + 56 * i, start + 0x100000 * i, 0x4000, 0x4000 * i, 0, 0, 0, 5, 5)
    return bytes(h)


# the shapes the 24A446 and 24B156 caches have: subcaches with 1 (70/72 files), 2 (4) and 8 (3) mappings, main with 1
for mc, end in ((1, 0x2a8), (2, 0x318), (8, 0x5b8)):
    h = header(mc)
    check(H.mapping_arrays_end(h) == end, '%d mapping(s): the arrays end at %#x' % (mc, end))
    check(not any(h[H.mapping_arrays_end(h):]), '%d mapping(s): nothing after the arrays (the zero tail)' % mc)
check(H.fixed_header_end(header(1)) == 0x238, 'the fixed header ends at 0x238 (24A446\'s 0x238)')
check(H.shared_region(header(1)) == (0x180000000, 0x17c228000), 'the shared region: 24A446\'s SHARED_START, SHARED_SIZE')
check(H.shared_region(header(1, size=0x187948000))[1] == 0x187948000, 'the shared region: 24B156\'s size')
old = bytearray(header(1))
struct.pack_into('<II', old, 0x10, 0x98, 1)        # an old header: no mappingWithSlide fields
check(H.mapping_arrays_end(bytes(old)) == 0x98 + 32, 'an old header (mappingOffset < 0x140): mapping_info[] only')
try:
    H.shared_region(bytes(0x100))
    check(False, 'a header without a shared region is refused')
except AssertionError:
    check(True, 'a header without a shared region is refused')
print('%d failed' % fails if fails else 'all ok')
sys.exit(1 if fails else 0)
