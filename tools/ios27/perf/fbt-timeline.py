#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
# fbt-timeline.py LOG [FROM_HHMMSS [T0_MS T1_MS]] : one frame timeline out of a phone's console log (docs/ios27/PACING.md).
#  - the kernel's event ring (the fork's sweetfb_trace, started with kern.hakios.sweetfb_trace_go=S): T tick, F frame taken, G its chains
#    done, P presented (number: the DECON's frame counter with dpu_probe=1), S SwapEnd called (chains queued), R recorded (released),
#    B/E the GPU queue started / ran empty (chains run), W/X a GPU wait by a client's thread (1 render thread, 2 render-server,
#    3 metal2vulkan, 4 other; +10 whole queue, +20 one chain);
#  - "BD:" lines of tools/ios27/perf/frame-observers/cawake.m (backboardd's CoreAnimation threads waking and sleeping);
#  - "PD:" lines of sbpage.m (SpringBoard's page offset set, O, and its main run loop W/Z).
# Prints the stages per frame (quantiles), the swaps per flick by size (runs), the page movement's own rate, and with T0/T1 the
# merged events of that span.
import re, sys, statistics as st
log = open(sys.argv[1], errors='replace').read().splitlines()
frm = sys.argv[2] if len(sys.argv) > 2 else None
# FBT
starts = [i for i, l in enumerate(log) if 'FBT: first event at mach_absolute_time' in l and (frm is None or l[:8] >= frm)]
if not starts: print('no FBT dump'); sys.exit(1)
i0 = starts[-1]
m = re.search(r'mach_absolute_time (\d+) \((\d+) us\)', log[i0]); A0 = int(m.group(1)); U0 = int(m.group(2))
ev = []
for l in log[i0 + 1:]:
    if 'FBT: done' in l: break
    if 'FBT:' not in l: continue
    for x in re.finditer(r' (\d+)([A-Z])(\d+)', l.split('FBT:', 1)[1]):
        ev.append((int(x.group(1)) / 1000.0, x.group(2), int(x.group(3))))
ev.sort(key=lambda e: e[0])
tb = U0 / A0 if A0 else 1  # us per tick
# PD (SpringBoard): abs base + us deltas
pd = []
for l in log:
    if 'PD: ' not in l or 'loaded' in l: continue
    s = l.split('PD: ', 1)[1].split()
    try: base = int(s[0])
    except Exception: continue
    for tok in s[1:]:
        x = re.match(r'(\d+)([A-Z])(-?\d+)', tok)
        if not x: continue
        t_us = base * tb + int(x.group(1)) - U0
        pd.append((t_us / 1000.0, x.group(2), int(x.group(3))))
lo, hi = ev[0][0], ev[-1][0]
pd = sorted(e for e in pd if lo <= e[0] <= hi)
print('FBT events %d over %.0f ms; PD events in that span %d' % (len(ev), hi - lo, len(pd)))
def q(xs):
    if not xs: return '-'
    xs = sorted(xs); n = len(xs)
    return 'n %d p10 %.1f p50 %.1f p90 %.1f max %.1f' % (n, xs[n // 10], xs[n // 2], xs[min(n - 1, 9 * n // 10)], xs[-1])
T = [t for t, w, a in ev if w == 'T']; S = [(t, a) for t, w, a in ev if w == 'S']; R = [t for t, w, a in ev if w == 'R']
P = [(t, a) for t, w, a in ev if w == 'P']; F = [(t, a) for t, w, a in ev if w == 'F']; G = [t for t, w, a in ev if w == 'G']
O = [t for t, w, a in pd if w == 'O']
print('ticks: period', q([b - a for a, b in zip(T, T[1:])]))
# motion runs: present gaps <= 3 ticks (50 ms)
Pt = [t for t, a in P]
runs = []; cur = [Pt[0]] if Pt else []
for a, b in zip(Pt, Pt[1:]):
    if b - a <= 50: cur.append(b)
    else:
        if len(cur) >= 6: runs.append(cur)
        cur = [b]
if len(cur) >= 6: runs.append(cur)
mot = sum(r[-1] - r[0] for r in runs); nfr = sum(len(r) - 1 for r in runs)
print('motion runs (presents <= 50 ms apart, >= 6): %d, %.0f ms, %d frames: %.1f fps in motion' % (len(runs), mot, nfr, 1000.0 * nfr / mot if mot else 0))
inrun = lambda t: any(r[0] - 20 <= t <= r[-1] for r in runs)
print('present gaps in motion (ms):', q([b - a for r in runs for a, b in zip(r, r[1:])]))
g = [round((b - a) / 16.667) for r in runs for a, b in zip(r, r[1:])]
print('  in refreshes: ' + ' '.join('%d:%d' % (k, g.count(k)) for k in sorted(set(g))))
St = [t for t, a in S if inrun(t)]
print('swap calls in motion: period', q([b - a for a, b in zip(St, St[1:]) if b - a < 60]))
# swap hold: S -> next R
hold = []
for t, a in S:
    r = next((x for x in R if x >= t), None)
    if r is not None and inrun(t): hold.append(r - t)
print('SwapEnd held', q(hold))
# tick took frame -> chains done -> presented
fg = []; gp = []; sp = []; dec = []
for t, a in F:
    if not inrun(t): continue
    gg = next((x for x in G if x >= t), None); pp = next(((x, d) for x, d in P if x >= t), None)
    if gg: fg.append(gg - t)
    if pp and gg: gp.append(pp[0] - gg)
    s = [x for x, sid in S if x <= t]
    if s and pp: sp.append(pp[0] - s[-1])
print('tick took frame -> its chains done (tick slept)', q(fg)); print('chains done -> present done', q(gp))
print('last swap call -> present done', q(sp))
dc = [d for t, d in P if inrun(t)]
if dc and dc[-1]:
    print('DECON refreshes between presents in motion:', ' '.join('%d:%d' % (k, [b - a for a, b in zip(dc, dc[1:])].count(k)) for k in range(0, 6)))
# GPU busy runs
B = [t for t, w, a in ev if w == 'B']; E = [(t, a) for t, w, a in ev if w == 'E']
busy = []
for b in B:
    e = next(((x, n) for x, n in E if x >= b), None)
    if e and inrun(b): busy.append((e[0] - b, e[1]))
print('GPU queue busy runs in motion: ms', q([x for x, n in busy]), '| chains', q([n for x, n in busy]))
tot = sum(x for x, n in busy); print('GPU queue busy share of motion: %.0f %%' % (100 * tot / mot if mot else 0))
if O:
    Om = [t for t in O if inrun(t)]
    print('SpringBoard offset sets in motion: period', q([b - a for a, b in zip(Om, Om[1:]) if b - a < 60]))
    # each offset -> next swap call
    os_ = []
    for t in Om:
        s = next((x for x, a in S if x >= t), None)
        if s is not None: os_.append(s - t)
    print('offset set -> next swap call', q(os_))
# backboardd's CoreAnimation threads (bd.dylib): W woke / Z about to sleep, arg = thread index
names = {}
for l in log:
    m = re.search(r"BD: observer on thread (\d+) '([^']+)'", l)
    if m: names[int(m.group(1))] = m.group(2).replace('com.apple.coreanimation.', '')
bd = []
for l in log:
    if 'BD: ' not in l: continue
    s = l.split('BD: ', 1)[1].split()
    try: base = int(s[0])
    except Exception: continue
    for tok in s[1:]:
        x = re.match(r'(\d+)([A-Z])(-?\d+)$', tok)
        if not x: continue
        t_us = base * tb + int(x.group(1)) - U0
        bd.append((t_us / 1000.0, x.group(2), int(x.group(3))))
bd = sorted(e for e in bd if lo <= e[0] <= hi)
print('BD events in the span: %d; threads %s' % (len(bd), names))
for idx, nm in sorted(names.items()):
    W = [t for t, w, a in bd if w == 'W' and a == idx]; Z = [t for t, w, a in bd if w == 'Z' and a == idx]
    Wm = [t for t in W if inrun(t)]
    print('  %-22s wakes in motion %d (%.1f/s): period %s' % (nm, len(Wm), 1000.0 * len(Wm) / mot if mot else 0, q([b - a for a, b in zip(Wm, Wm[1:]) if b - a < 60])))
    busy = []
    for t in Wm:
        z = next((x for x in Z if x >= t), None)
        if z is not None: busy.append(z - t)
    print('  %-22s awake per wake in motion: %s' % ('', q(busy)))
    # wake -> next swap call (for the render thread)
    if 'display' in nm:
        ws = []
        for t in Wm:
            s_ = next((x for x, a in S if x >= t), None)
            if s_ is not None and s_ - t < 40: ws.append(s_ - t)
        print('  %-22s wake -> swap call: %s (%d of %d wakes end in a swap within 40 ms)' % ('', q(ws), len(ws), len(Wm)))

# swaps in runs (<= 60 ms apart): refreshes since the swap before, for big (>= 10 chains) and small swaps
Sx = [(t, a) for t, w, a in ev if w == 'S']
rr = []; cur = Sx[:1]
for p, c in zip(Sx, Sx[1:]):
    if c[0] - p[0] <= 60: cur.append(c)
    else: rr.append(cur); cur = [c]
rr.append(cur)
big = []; small = []
for r in rr:
    if len(r) < 5: continue
    for p, c in zip(r, r[1:]): (big if c[1] >= 10 else small).append(round((c[0] - p[0]) / 16.667))
h = lambda xs: ' '.join('%d:%d' % (k, xs.count(k)) for k in sorted(set(xs)))
print('swaps in runs, refreshes since the swap before: >= 10 chains %s | < 10 chains %s' % (h(big), h(small)))
if big: print('  big swaps: %.2f refreshes each = %.1f a second' % (sum(big) / len(big), 60.0 * len(big) / sum(big)))
# the page movement: SpringBoard's offset runs (<= 60 ms apart, >= 4) and the swaps inside them
Op = [(t, a) for t, w, a in pd if w == 'O']
if Op:
    orr = []; cur = Op[:1]
    for p, c in zip(Op, Op[1:]):
        if c[0] - p[0] <= 60: cur.append(c)
        else: orr.append(cur); cur = [c]
    orr.append(cur)
    no = ns = 0; tt = 0.0; po = []; ps = []
    for r in orr:
        if len(r) < 4: continue
        a, b = r[0][0], r[-1][0]; ss = [t for t, x in Sx if a <= t <= b + 5]
        no += len(r) - 1; ns += len(ss); tt += b - a
        po += [y[0] - x[0] for x, y in zip(r, r[1:])]; ps += [y - x for x, y in zip(ss, ss[1:])]
    if tt: print('page movement: %.0f ms: SpringBoard offsets %.1f a second (%s), swaps %.1f a second (%s)' % (tt, 1000.0 * no / tt, q(po), 1000.0 * ns / tt, q(ps)))
# GPU waits by thread (W/X)
Wt = {}
for t, w, a in ev:
    if w == 'W': Wt.setdefault(a, []).append(t)
for a, ts in sorted(Wt.items()):
    xs = [t for t, w, b in ev if w == 'X' and b == a]
    d = []
    for t in ts:
        x = next((y for y in xs if y >= t), None)
        if x is not None: d.append(x - t)
    print('GPU waits by %s (%s): %d, %s, total %.0f ms' % ({1: 'the render thread', 2: 'render-server', 3: 'metal2vulkan', 4: 'another thread'}.get(a % 10, '?'),
          {0: 'one buffer', 1: 'whole queue', 2: 'one chain'}.get(a // 10, '?'), len(d), q(d), sum(d)))
def dump(t0, t1):
    allev = sorted([(t, w, a, 'K') for t, w, a in ev] + [(t, w, a, 'B') for t, w, a in bd] + [(t, w, a, 'P') for t, w, a in pd])
    prev = None
    for t, w, a, src in allev:
        if t0 <= t <= t1:
            lab = (names.get(a, str(a))[:14] if src == 'B' else '')
            print('%9.2f %+6.2f %s %s%d %s' % (t, t - (prev or t), src, w, a, lab)); prev = t
if len(sys.argv) > 3: dump(float(sys.argv[3]), float(sys.argv[4]))
