#!/usr/bin/env python3
# prof.py LOG KERNEL T0 T1 [TOPN] : aggregate SWEET PROF samples (kernel boot-arg sweetspeed&16384) from a timestamped console extract
# (lines "EPOCH text"); symbolicate shared cache pcs (dscsym index), kernel pcs (nm of KERNEL),
# non-cache mappings (SWEET PROF map lines + nm of the matching local file when known).
import sys, re, os, bisect, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscsym
if len(sys.argv) < 5:
    sys.exit(__doc__)
LOG, KERN, T0, T1 = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
TOPN = int(sys.argv[5]) if len(sys.argv) > 5 else 60
# non-cache libraries with symbols of your own build: SYMFILES="libsbgl.dylib=/path/unstripped,libOSMesa.8.dylib=/path/unstripped"
LOCAL = dict(kv.split('=', 1) for kv in os.environ.get('SYMFILES', '').split(',') if '=' in kv)
def nm_index(path, slide=0):
    out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
    a, n = [], []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[1] in 'tTsS':
            a.append(int(p[0], 16) + slide); n.append(p[2])
    return a, n
ka, kn = nm_index(KERN)
def ksym(pc):
    i = bisect.bisect_right(ka, pc) - 1
    return kn[i] if i >= 0 else '?'
m3c = collections.Counter(); slide = None; names = {}; maps = []; samples = collections.Counter(); hdr = []
emu = []
for ln in open(LOG, errors='replace'):
    try: t = float(ln[:13])
    except: continue
    s = ln[14:].rstrip()
    m = re.search(r'SMP \S+\[\d+\] slide 0x([0-9a-f]+)', s)
    if m and slide is None: slide = int(m.group(1), 16)
    for m in re.finditer(r' (\S+)\[(\d+)\] \d+/\d+/m', s): names[int(m.group(2))] = m.group(1)
    for m in re.finditer(r'(\S+)\[(\d+)\] \d+%', s): names[int(m.group(2))] = m.group(1)
    m = re.match(r'SWEET PROF map: (\d+) ([0-9a-f]+)-([0-9a-f]+) off ([0-9a-f]+) (.*)', s)
    if m: maps.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), m.group(5)))
    if not (T0 <= t <= T1): continue
    if s.startswith('SWEET PROF ('): hdr.append(s)
    if s.startswith('SWEET PROF emu:'): emu.append(s)
    if s.startswith('SWEET PROF pcs:'):
        for m in re.finditer(r' ([ukUK])(\d+):([0-9a-f]+):(\d+)', s):
            k = m.group(1).lower(); big = m.group(1).isupper()
            samples[(k, int(m.group(2)), int(m.group(3), 16))] += int(m.group(4))
            m3c[(names.get(int(m.group(2)), 'pid' + m.group(2)), big)] += int(m.group(4))
tot = 0
for h in hdr:
    m = re.search(r': (\d+) samples', h); tot += int(m.group(1))
print('windows', len(hdr), 'total samples', tot, 'in pcs>=3:', sum(samples.values()), 'slide', hex(slide or 0))
for e in emu: pass
el = [re.findall(r'(\d+) x (\d+) ns = (\d+) ms', e) for e in emu]
if el:
    lse = sum(int(x[0][2]) for x in el if x); a64 = sum(int(x[1][2]) for x in el if len(x) > 1)
    ln_ = sum(int(x[0][0]) for x in el if x); an = sum(int(x[1][0]) for x in el if len(x) > 1)
    print('emulator time: lse %d traps %d ms, a64e %d traps %d ms' % (ln_, lse, an, a64))
S = dscsym.load(None) if os.path.exists(os.environ.get('DSCSYM_INDEX', 'dscsym.pickle')) else None
local_idx = {}
def usym(pid, pc):
    if 0x180000000 <= pc < 0x400000000 and slide is not None and S:
        img, nm, off = S.lookup(pc - slide); return img, nm
    for p, a, b, o, path in maps:
        if p == pid and a <= pc < b:
            base = os.path.basename(path)
            lp = LOCAL.get(base)
            if lp and os.path.exists(lp):
                if lp not in local_idx: local_idx[lp] = nm_index(lp)
                aa, nn = local_idx[lp]; v = pc - a + o
                i = bisect.bisect_right(aa, v) - 1
                return base, nn[i] if i >= 0 else '?'
            return base, '+%x' % (pc - a + o)
    return '?', hex(pc)
proc = collections.Counter(); func = collections.Counter(); img = collections.Counter()
for (k, pid, pc), c in samples.items():
    pn = names.get(pid, 'pid%d' % pid)
    if k == 'k':
        fn = ksym(pc); im = 'kernel'
    else:
        im, fn = usym(pid, pc)
    proc[(pn, k)] += c; img[(pn, im)] += c; func[(pn, im, fn)] += c
P = lambda c: 100.0 * c / max(1, tot)
print('\n== by process (user/kernel) % of all samples')
pp = collections.Counter()
for (pn, k), c in proc.items(): pp[pn] += c
for pn, c in pp.most_common(15):
    print('%6.2f%%  %-28s u %5.2f k %5.2f  on M3 %3.0f%%' % (P(c), pn, P(proc[(pn, 'u')]), P(proc[(pn, 'k')]), 100.0 * m3c[(pn, True)] / max(1, m3c[(pn, True)] + m3c[(pn, False)])))
print('\n== by process/image')
for (pn, im), c in img.most_common(40): print('%6.2f%%  %-20s %s' % (P(c), pn, im))
print('\n== by function')
for (pn, im, fn), c in func.most_common(TOPN): print('%6.2f%%  %-16s %-26s %s' % (P(c), pn[:16], im[:26], fn[:110]))
