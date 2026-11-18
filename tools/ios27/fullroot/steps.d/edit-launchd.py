#!/usr/bin/env python3
"""edit-launchd.py ROOT LISTS_DIR [--set bp|min] [--hogs] [--keep LABEL,...] : launchd job edits of the full root (declarative + idempotent: everything
previously moved to launchd-disabled.plist is put back first, then the chosen set is moved out again).
  1. hardware list (daemons-hw.list): jobs removed from launchd.plist, kept in launchd-disabled.plist (restorable)
  2. --set min : crash-loop list (daemons-crashloop.list) [+ daemons-hogs.list with --hogs], by exact Label
     --set bp  : the default: daemons-bp.keys, the hardware-less jobs only (until 18 Nov 2026 also crash loops, the 84 speed trims and CPU hogs)
     --keep   : jobs (exact Label, or exact job key) left in launchd.plist although 1. or 2. would move them out; empty by
                default (no change). For a test of what a removed daemon does, e.g. com.apple.siriactionsd for Control Centre's
                controls (docs/ios27/CONTROL-CENTRE.md); a keep that matches no job is said.
  3. _PanicOnCrash dropped from every job (a crash loop must never panic the kernel on this port)
  4. launchd-override list -> /private/var/db/com.apple.xpc.launchd/disabled.plist
"""
import os, plistlib, sys
root, lists = sys.argv[1], sys.argv[2]
hogs = '--hogs' in sys.argv
DSET = sys.argv[sys.argv.index('--set') + 1] if '--set' in sys.argv else 'min'
KEEP = set(x for x in (sys.argv[sys.argv.index('--keep') + 1] if '--keep' in sys.argv else '').replace(',', ' ').split() if x)
assert DSET in ('bp', 'min'), DSET
def rd(p):
    return [l.split('#')[0].strip() for l in open(p) if l.split('#')[0].strip()]
lp = root + '/System/Library/xpc/launchd.plist'; dp = root + '/System/Library/xpc/launchd-disabled.plist'
raw = open(lp, 'rb').read(); d = plistlib.loads(raw)
try: dis = plistlib.load(open(dp, 'rb'))
except FileNotFoundError: dis = {}
jobs = d['LaunchDaemons']
for k, v in dis.items(): jobs.setdefault(k, v)      # declarative: start from every job the image has
dis = {}
hw = rd(lists + '/daemons-hw.list')
exact = set(rd(lists + '/daemons-crashloop.list')) | (set(rd(lists + '/daemons-hogs.list')) if hogs else set()) if DSET == 'min' else set()
keys = set(rd(lists + '/daemons-bp.keys')) if DSET == 'bp' else set()
moved = []; kept = []
for k in list(jobs):
    lab = jobs[k].get('Label', '') if isinstance(jobs[k], dict) else ''
    if any(h in k or h in lab for h in hw) or lab in exact or k in keys:
        if lab in KEEP or k in KEEP:
            kept.append(lab or k); continue
        dis[k] = jobs.pop(k); moved.append(lab or k)
pc = 0
for k, j in jobs.items():
    if isinstance(j, dict) and '_PanicOnCrash' in j:
        del j['_PanicOnCrash']; pc += 1
with open(lp, 'r+b') as f:
    f.seek(0); f.write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); f.truncate()
with open(dp, 'wb') as f:
    f.write(plistlib.dumps(dis, fmt=plistlib.FMT_BINARY))
os.chmod(dp, 0o644); os.chown(dp, 0, 0)
print(f'launchd.plist: {len(moved)} jobs moved to launchd-disabled.plist ({len(jobs)} left), _PanicOnCrash dropped from {pc}')
print('  moved:', ' '.join(sorted(moved)))
if KEEP:
    print('  kept on (--keep):', ' '.join(sorted(kept)) or '-')
    lk = {(v.get('Label') if isinstance(v, dict) else None) for v in jobs.values()} | set(jobs)
    nok = sorted(x for x in KEEP if x not in lk)
    if nok: print('  --keep names no job of this root:', ' '.join(nok))
miss = sorted(x for x in exact if x not in {v.get('Label') for v in dis.values()})
if miss: print('  not on this root:', ' '.join(miss))
ov = root + '/private/var/db/com.apple.xpc.launchd/disabled.plist'
os.makedirs(os.path.dirname(ov), exist_ok=True)
try: o = plistlib.load(open(ov, 'rb'))
except FileNotFoundError: o = {}
for l in rd(lists + '/launchd-override-disabled.list'):
    lab, val = l.split(); o[lab] = (val == 'true')
open(ov, 'wb').write(plistlib.dumps(o, fmt=plistlib.FMT_BINARY)); os.chmod(ov, 0o644); os.chown(ov, 0, 0)
print('disabled.plist overrides:', len(o))
