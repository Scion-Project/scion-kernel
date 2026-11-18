#!/usr/bin/env python3
# fbt-regimes.py LOG: the swaps of a sweetfb_trace ring (the FBT: lines of the fork's SweetFramebuffer.cpp) split by what the GPU
# queue was doing. A GPU-busy run is from B (the queue gets a chain after being empty) to E (it runs empty again); per run: its
# length, chains and swaps, so the app GPU queue time a swap. A swap is "busy" when it was held (SwapEnd waited more than 1 ms for
# the swap before it) or more than 8 chains were queued at its call, else "idle". Per class: the swap-call period, the SwapEnd
# hold, the time from the vsync tick before the call to the call (the render's CPU time), the chains queued at the call.
import re, sys, statistics as st
ev = []
for line in open(sys.argv[1], errors="replace"):
    if "FBT:" not in line or "done" in line: continue
    for t, w, a in re.findall(r" (\d+)([A-Z])(\d+)", line.split("FBT:", 1)[1]):
        ev.append((int(t) / 1000.0, w, int(a)))
def q(x):
    if not x: return "n=0"
    x = sorted(x); return "n=%d p50 %.1f avg %.1f p90 %.1f" % (len(x), x[len(x) // 2], st.mean(x), x[int(len(x) * .9)])
busy = False; start = None; runs = []; lastT = None; lastS = None
cls = {"busy": [], "idle": []}
cur = []
first_B_seen = False
for i, (t, w, a) in enumerate(ev):
    if w == "B": busy = True; start = t; cur = []
    if w == "T": lastT = t
    if w == "S":
        R = next((e for e in ev[i:] if e[1] == "R"), None)
        hold = R[0] - t if R else 0
        per = t - lastS if lastS is not None else None
        lastS = t
        # a swap is "busy" when chains of an earlier swap were still on the queue at its call (it had to wait, or could have)
        k = "busy" if hold > 1.0 or a > 8 else "idle"
        cls[k].append((per, hold, t - lastT if lastT else None, a))
        cur.append(t)
    if w == "E":
        if start is not None and len(cur) >= 3:
            runs.append((start, t, a, len(cur)))
        busy = False; start = None
for s, e, n, k in runs:
    print("GPU-busy run %.0f..%.0f ms: %.0f ms, %d chains, %d swaps: %.1f ms of app GPU queue a swap" % (s, e, e - s, n, k, (e - s) / k))
for k in ("busy", "idle"):
    xs = cls[k]
    print("%s swaps: period %s | hold %s | tick->call %s | chains at call %s" % (k, q([x[0] for x in xs if x[0] and x[0] < 200]),
        q([x[1] for x in xs]), q([x[2] for x in xs if x[2] is not None]), q([x[3] for x in xs])))
