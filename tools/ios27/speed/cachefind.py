#!/usr/bin/env python3
# cachefind.py CACHEDIR VADDR : which subcache file of an arm64e dyld shared cache holds an unslid address (prints its path;
# exit 1 when none does). For saving that file before cachepatch.py changes it.
import glob, struct, sys
if len(sys.argv) != 3:
    sys.exit(__doc__)
d, va = sys.argv[1], int(sys.argv[2], 16)
for p in sorted(glob.glob(d + '/dyld_shared_cache_arm64e*')):
    if p.endswith(('.map', '.atlas', '.symbols')):
        continue
    h = open(p, 'rb').read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', h, 0x10)
    with open(p, 'rb') as f:
        f.seek(mo)
        for _ in range(mc):
            a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
            if a <= va < a + s:
                print(p)
                sys.exit(0)
sys.exit(1)
