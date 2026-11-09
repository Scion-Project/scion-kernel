#!/usr/bin/env python3
# summ.py LOG START : per-boot summary of a boot with the profiler (sweetspeed&16384) and the scripted bench (&4096): bench fps, CPU by
# process and core type, GPU chains, time in the emulator. LOG as for ../speed/bench.py.
import re, sys, os, collections, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if len(sys.argv) < 3:
    sys.exit(__doc__)
LOG = sys.argv[1]; S0 = float(sys.argv[2])
out = subprocess.run(['python3', os.path.dirname(os.path.abspath(__file__)) + '/../speed/bench.py', LOG, str(S0)], capture_output=True, text=True).stdout
print('\n'.join(out.splitlines()[:10]))
L = []
for ln in open(LOG, errors='replace'):
    try: t = float(ln[:13])
    except: continue
    if t > S0: L.append((t, ln[14:].rstrip()))
T0 = next(t for t, l in L if 'first frame on the panel' in l)
sw = {}
for t, l in L:
    m = re.search(r'scripted swipe (\d+):', l)
    if m and int(m.group(1)) not in sw: sw[int(m.group(1))] = t
import statistics as st
def fpsw(a, b, name):
    v = sorted(float(m.group(1)) for t, l in L if a <= t < b for m in [re.search(r'A5XELTE FB: .*fps=([\d.]+)', l)] if m)
    if v: print('%-7s fps n=%d median %.1f p90 %.1f mean %.1f' % (name, len(v), st.median(v), v[min(len(v)-1, int(.9*len(v)))], st.mean(v)))
if 1 in sw and 16 in sw:
    fpsw(sw[1] + 2, sw[16] + 3, 'SWIPES'); fpsw(sw[19] + 2, sw[34] + 3, 'SCROLL') if 34 in sw else None
import dscsym
Sy = dscsym.load(None)
def window(a, b, name):
    u = collections.Counter(); k = collections.Counter(); m3 = collections.Counter(); S = A = M = W = 0
    img = collections.Counter(); sl = None; gpu = [0, 0.0]; top = collections.Counter(); topn = 0
    emu = [0, 0, 0, 0]
    for t, s in L:
        m = re.search(r'SMP \S+\[\d+\] slide 0x([0-9a-f]+)', s)
        if m: sl = int(m.group(1), 16)
        if not (T0 + a <= t <= T0 + b): continue
        m = re.search(r'SWEET PROF \(10 s\): (\d+) samples.*?a55 (\d+) m3 (\d+)', s)
        if m:
            W += 1; S += int(m.group(1)); A += int(m.group(2)); M += int(m.group(3))
            for x in re.finditer(r' (\S+)\[(\d+)\] (\d+)/(\d+)/m(\d+)', s):
                n = x.group(1); u[n] += int(x.group(3)); k[n] += int(x.group(4)); m3[n] += int(x.group(5))
        m = re.search(r'GPU ASYNC \(10 s\): (\d+) chains, gpu avg (\d+) us', s)
        if m: gpu[0] += int(m.group(1)); gpu[1] += int(m.group(1)) * int(m.group(2)) / 1e6
        m = re.search(r'STARLTE TOP \(10 s, all (\d+)%\):(.*)', s)
        if m:
            topn += 1
            for x in re.finditer(r' (\S+)\[\d+\] (\d+)%', m.group(2)): top[x.group(1)] += int(x.group(2))
        m = re.search(r'SWEET PROF emu: lse (\d+) x \d+ ns = (\d+) ms, a64e (\d+) x \d+ ns = (\d+) ms', s)
        if m: emu = [e + int(g) for e, g in zip(emu, m.groups())]
    if not W: print(name, 'no profile windows'); return
    print('== %s (+%d..+%d s): %d windows, %.2f cores busy (A55 %.2f, M3 %.2f); GPU %d chains/s, %.0f%% busy; emu lse %d/%d ms a64e %d/%d ms' % (
        name, a, b, W, S / (W * 1e4), A / (W * 1e4), M / (W * 1e4), gpu[0] / (W * 10), 100 * gpu[1] / (W * 10), emu[0], emu[1], emu[2], emu[3]))
    print('   TOP %%CPU avg: ' + ', '.join('%s %.0f' % (n, c / max(1, topn)) for n, c in top.most_common(6)))
    for n, c in sorted(((n, u[n] + k[n]) for n in u), key=lambda x: -x[1])[:6]:
        print('   %-18s %.2f cores (user %.2f kern %.2f) on M3 %3.0f%%' % (n, c / (W * 1e4), u[n] / (W * 1e4), k[n] / (W * 1e4), 100.0 * m3[n] / max(1, c)))
if 1 in sw and 34 in sw:
    window(sw[1] - T0, sw[16] + 3 - T0, 'swipes'); window(sw[19] - T0, sw[34] + 3 - T0, 'scroll')
else:
    window(62, 93, 'swipes'); window(102, 133, 'scroll')
