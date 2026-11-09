#!/usr/bin/env python3
"""mtl-cmdline.py BASE_CMDLINE OUT [--posters]: your current cmdline + Metal .
Drops defaults/no-ops to stay under the shim's 608 bytes: sweettrace=0, sweettracename=none (defaults),
unentitled_ios_sim_launch=1 (no-op on the iOS-target k3/k4 kernels). Adds sbmtl=1 (StarbiteAccelerator ->
IOAcceleratorES with MetalPluginName StarbiteMetal). --posters: also inject /usr/lib/sbmtlc.dylib (empty lib ->
ptrauth_disabled=1, so the arm64 Metal plug-in loads) into PosterBoard and CollectionsPoster.
--metal-ca: + sbgl=0 (CoreAnimation renders with Metal in backboardd; put backboardd in mtl2-clients)."""
import sys
if len(sys.argv) < 3: sys.exit(__doc__)
c = open(sys.argv[1]).read().strip()
for drop in (' sweettrace=0', ' sweettracename=none', ' unentitled_ios_sim_launch=1', ' sbmtl=1'):
    c = c.replace(drop, '')
c += ' sbmtl=1'
if '--posters' in sys.argv:
    words = c.split(' ')
    for i, w in enumerate(words):
        if w.startswith('sweetinject='):
            words[i] = w + ';/usr/lib/sbmtlc.dylib,PosterBoard,CollectionsPoster'
            break
    else:
        words.append('sweetinject=/usr/lib/sbmtlc.dylib,PosterBoard,CollectionsPoster')
    c = ' '.join(words)
if '--metal-ca' in sys.argv:
    # CoreAnimation's own Metal renderer in backboardd (Liquid Glass needs it): sbca stays injected for its
    # iOS 27 patches (BGRA framebuffer, push_surface, EDR client) but leaves the renderer alone with sbgl=0.
    # Also needs "backboardd" in /private/var/local/mtl2-clients.
    c = c.replace(' sbgl=1', '') + ' sbgl=0'
    if len(c) >= 608:
        c = c.replace(',CollectionsPoster', '')
assert len(c) < 608, len(c)
open(sys.argv[2], 'w').write(c + '\n')
print(len(c), c)
