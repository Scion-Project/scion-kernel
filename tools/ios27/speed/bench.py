#!/usr/bin/env python3
# bench.py LOG START_EPOCH [END_EPOCH] : summarize one boot of an iOS 27 S9 from a timestamped console log (docs/ios27/NIGHT.md).
# LOG lines are "<epoch seconds, 13 characters> <console line>" (a reader that stamps the USB console's lines); START_EPOCH is the
# time of the reboot command. Frame rates come from the "A5XELTE FB: ... fps=" lines of the framebuffer (every 5 s) in the windows
# of the scripted input (kernel boot-arg sweetspeed&4096: starlte_touch.c script 5, times from the first frame: Home 45 s, 16 home
# screen swipes 60-90 s, Settings tapped at 95 s, 16 scroll swipes 100-130 s, Home 135 s); a window with no frame prints
# "no samples" (a static screen). Also: exec counts of a few daemons, crashes, self-sent SIGKILLs (the sweetspeed&32 log), and the
# prewarm / async-GPU / failure lines.
import os, re, statistics as st, sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
S = float(sys.argv[2]); E = float(sys.argv[3]) if len(sys.argv) > 3 else 1e18
L = []
for ln in open(sys.argv[1], errors='replace'):
    try:
        t = float(ln[:13])
    except ValueError:
        continue
    if S < t < E:
        L.append((t, ln[14:].rstrip()))
if not L:
    sys.exit('no lines in the window')
T0 = next((t for t, l in L if 'first frame on the panel' in l), None)
print('lines', len(L), 'first frame at +%.1f s from the first log line' % ((T0 - L[0][0]) if T0 else -1))
fps = [(t, float(m.group(1))) for t, l in L for m in [re.search(r'A5XELTE FB: .*fps=([\d.]+)', l)] if m]


def stat(name, a, b):
    v = [f for t, f in fps if T0 and T0 + a <= t < T0 + b]
    if not v:
        print(name, 'no samples')
        return
    v2 = sorted(v)
    p90 = v2[min(len(v2) - 1, int(0.9 * len(v2)))]
    p10 = v2[int(0.1 * len(v2))]
    print('%-8s n=%2d median %.1f p10 %.1f p90 %.1f mean %.1f' % (name, len(v), st.median(v), p10, p90, st.mean(v)))


stat('home', 45, 62); stat('swipes', 62, 93); stat('settings', 95, 102); stat('scroll', 102, 133)
stat('bench', 45, 135); stat('idle', 140, 400)
ex = {}
for t, l in L:
    m = re.search(r'\bexec (\S+)', l)
    if m:
        ex[m.group(1)] = ex.get(m.group(1), 0) + 1
print('execs', {k: ex[k] for k in ['SpringBoard', 'backboardd', 'runningboardd', 'lsd', 'configd', 'Preferences', 'locationd'] if k in ex})
cr = {}
for t, l in L:
    m = re.match(r'CRASH (\S+)\[\d+\] sig', l)
    if m:
        cr[m.group(1)] = cr.get(m.group(1), 0) + 1
print('crashes', cr)
sk = {}
for t, l in L:
    m = re.match(r'SWEET SIG: (\S+)\[\d+\] -> (\S+)\[\d+\] sig 9 by psignal', l)
    if m and m.group(1) == m.group(2):
        sk[m.group(1)] = sk.get(m.group(1), 0) + 1
print('self-SIGKILL', sum(sk.values()), dict(sorted(sk.items(), key=lambda x: -x[1])[:15]))
for t, l in L:
    if re.search(r'PREWARM|scripted input done|GPU ASYNC|FAILED|GPU reset|panic|recommended cores|canary|SWEET PROF: on', l):
        print('%.1f %s' % (t - (T0 or S), l[:200]))
