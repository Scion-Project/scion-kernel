#!/usr/bin/env python3
# costtab.py LOG START KERNEL [PROC] : where PROC's SWEET PROF samples (sweetspeed&16384) go in the swipes window of the scripted bench,
# grouped by component. KERNEL: the unstripped kernel the phone ran (for nm). The shared cache symbols: $DSCSYM_INDEX (dscsym.py).
import re, sys, os, bisect, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscsym
if len(sys.argv) < 4:
    sys.exit(__doc__)
LOG = sys.argv[1]; S0 = float(sys.argv[2]); KERN = sys.argv[3]; PROC = sys.argv[4] if len(sys.argv) > 4 else 'backboardd'
out = subprocess.run(['nm', '-n', KERN], capture_output=True, text=True).stdout
ka, kn = [], []
for l in out.splitlines():
    p = l.split()
    if len(p) >= 3 and p[1] in 'tTsS': ka.append(int(p[0], 16)); kn.append(p[2])
Sy = dscsym.load(None)
L = []
for ln in open(LOG, errors='replace'):
    try: t = float(ln[:13])
    except: continue
    if t > S0: L.append((t, ln[14:]))
T0 = next(t for t, l in L if 'first frame on the panel' in l)
pid = None; sl = None; maps = []; c = collections.Counter(); fn = collections.Counter(); tot = 0
for t, s in L:
    m = re.search(r'SMP %s\[(\d+)\] slide 0x([0-9a-f]+)' % PROC, s)
    if m: pid = int(m.group(1)); sl = int(m.group(2), 16)
    m = re.match(r'SWEET PROF map: (\d+) ([0-9a-f]+)-([0-9a-f]+) off ([0-9a-f]+) (.*)', s)
    if m: maps.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), m.group(5).strip()))
    if not (T0 + 62 <= t <= T0 + 93) or pid is None: continue
    if 'SWEET PROF pcs' not in s: continue
    for x in re.finditer(r' ([ukUK])(\d+):([0-9a-f]+):(\d+)', s):
        if int(x.group(2)) != pid: continue
        pc = int(x.group(3), 16); n = int(x.group(4)); k = x.group(1).lower(); tot += n
        if k == 'k':
            i = bisect.bisect_right(ka, pc) - 1; f = kn[i] if i >= 0 else '?'
            if re.search(r'sweet_a64e|sweet_lse|sleh_|emulate|fp16|a55', f): g = 'kernel: arm64e/LSE/FP16 emulation + trap'
            elif re.search(r'StarbiteMali|starlte_gpu|sgu_|sbm_|IOUserClient|io_connect|is_io_|IOExternal', f): g = 'kernel: GPU driver + IOKit calls'
            elif re.search(r'ipc_|mach_msg|kmsg|port|mig', f): g = 'kernel: Mach IPC'
            elif re.search(r'vm_|pmap|fault|zfill|copyin|copyout|bcopy|memmove', f): g = 'kernel: VM / copies'
            elif re.search(r'zalloc|zfree|zone|kalloc|kfree|bzero|memcmp_zero', f): g = 'kernel: allocator / zeroing'
            elif re.search(r'interrupts_enabled|lck_|lock|ticket|preempt', f): g = 'kernel: locks / irq-off sections'
            else: g = 'kernel: other'
        elif 0x180000000 <= pc < 0x400000000:
            img, f, _ = Sy.lookup(pc - sl); g = 'user: ' + img
        else:
            g = 'user: ?'; f = hex(pc)
            for p, a, b, path in maps:
                if p == pid and a <= pc < b: g = 'user: ' + os.path.basename(path); f = '+%x' % (pc - a)
        c[g] += n; fn[(g, f)] += n
print('%s pid %s: %d samples in swipes window' % (PROC, pid, tot))
for g, n in c.most_common(18): print('%5.1f%%  %s' % (100.0 * n / tot, g))
print('-- top functions')
for (g, f), n in fn.most_common(25): print('%5.1f%%  %-34s %s' % (100.0 * n / tot, g[:34], f[:90]))
