#!/usr/bin/env python3
# gaps.py LOG START : present-gap histogram (vsync ticks between presented frames) in the windows of the scripted bench
# (sweetspeed&4096 for the input, &16384 for the "A5XELTE FB gaps:" lines). LOG: lines "<epoch, 13 characters> <console line>".
# "on time" = share of frames one tick after the previous; "motion fps" = 60 * frames / ticks over the frames with gaps 1-3.
import re, sys, os
if len(sys.argv) < 3:
    sys.exit(__doc__)
S0 = float(sys.argv[2]); L = []
for ln in open(sys.argv[1], errors='replace'):
    try: t = float(ln[:13])
    except: continue
    if t > S0: L.append((t, ln[14:]))
T0 = next(t for t, l in L if 'first frame on the panel' in l)
for name, a, b in (('swipes', 62, 93), ('scroll', 102, 133)):
    g = [0] * 5
    for t, l in L:
        m = re.search(r'FB gaps: 1:(\d+) 2:(\d+) 3:(\d+) 4-6:(\d+) 7\+:(\d+)', l)
        if m and T0 + a <= t <= T0 + b:
            g = [x + int(y) for x, y in zip(g, m.groups())]
    moving = g[0] + g[1] + g[2]
    if moving:
        print('%-6s gaps 1:%d 2:%d 3:%d 4-6:%d 7+:%d | on time %.0f%%, motion fps %.1f' % (name, *g, 100.0 * g[0] / moving,
              60.0 * moving / (g[0] + 2 * g[1] + 3 * g[2])))
