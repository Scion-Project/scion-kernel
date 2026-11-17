# SPDX-License-Identifier: MIT-0
"""dsc_header.py: the few facts of a dyld shared cache's header that rewrite-grow.py needs, read from the header itself
rather than written in as one build's numbers. Plain struct reads (no numpy), so CI can test them (dsc_header_test.py).

  shared_region(hdr)       (sharedRegionStart, sharedRegionSize) of the main cache's header (fields at 0xe0 and 0xe8)
  fixed_header_end(hdr)    where the fixed header ends and the mapping array begins (mappingOffset, at 0x10)
  mapping_arrays_end(hdr)  the first byte after mapping_info[] and mapping_and_slide_info[]: the header's zero tail starts there

On iOS 27.0.1 (24A446) and 27.2.1 (24B156) these give 0x180000000 and 0x17c228000 / 0x187948000, 0x238, and 0x2a8 for every
subcache with one mapping: the numbers rewrite-grow.py had written in for 24A446 (docs/ios27/UPDATING-IOS.md)."""
import struct

MAPPING_INFO = 32               # dyld_cache_mapping_info: address, size, fileOffset, maxProt, initProt
MAPPING_AND_SLIDE_INFO = 56     # dyld_cache_mapping_and_slide_info


def shared_region(hdr):
    start, size = struct.unpack_from('<QQ', hdr, 0xe0)
    assert start and size, 'no sharedRegionStart/Size in this header'
    return start, size


def fixed_header_end(hdr):
    return struct.unpack_from('<I', hdr, 0x10)[0]


def mapping_arrays(hdr):
    """(mappingOffset, mappingCount, mappingWithSlideOffset, mappingWithSlideCount); the last two 0 on an old header."""
    mo, mc = struct.unpack_from('<II', hdr, 0x10)
    wo = wc = 0
    if mo >= 0x140:                 # header new enough to carry mappingWithSlideOffset/Count
        wo, wc = struct.unpack_from('<II', hdr, 0x138)
    return mo, mc, wo, wc


def mapping_arrays_end(hdr):
    mo, mc, wo, wc = mapping_arrays(hdr)
    return max(mo + MAPPING_INFO * mc, wo + MAPPING_AND_SLIDE_INFO * wc if wc else 0)
