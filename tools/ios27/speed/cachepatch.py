#!/usr/bin/env python3
#!/usr/bin/env python3
# cachepatch.py CACHEDIR VADDR EXPECT_HEX NEW_HEX [--dry-run] : patch one 32-bit word at an unslid cache address
# of an arm64e dyld shared cache (the files CACHEDIR/dyld_shared_cache_arm64e*), only if the word there is EXPECT_HEX
# (exit 1 otherwise), and say so when it is NEW_HEX already. The word's address is the builder's own to find in their cache.
import glob, struct, sys
d, va, exp, new = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
dry = '--dry-run' in sys.argv
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
                off = o + va - a
                with open(p, 'r+b' if not dry else 'rb') as g:
                    g.seek(off); cur = struct.unpack('<I', g.read(4))[0]
                    print(f'{p} +{off:#x}: {cur:08x}', end=' ')
                    if cur == new:
                        print('already patched'); sys.exit(0)
                    if cur != exp:
                        print(f'!= expected {exp:08x}; not patched'); sys.exit(1)
                    if not dry:
                        g.seek(off); g.write(struct.pack('<I', new))
                    print('->', f'{new:08x}', '(dry run)' if dry else 'patched')
                    sys.exit(0)
print('address not mapped'); sys.exit(1)
