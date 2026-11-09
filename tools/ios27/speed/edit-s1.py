#!/usr/bin/env python3
# edit-s1.py VOL LIST : the daemon edits of the S9 speed work (idempotent). Moves the launchd jobs whose Label LIST names
# (one per line, # comments: trim-s1.list) from VOL/System/Library/xpc/launchd.plist into launchd-disabled.plist, and takes
# backboardd out of VOL/private/var/local/mtl2-clients (the Metal clients of tools/ios27/metal/install-metal27.py) when that file is there.
# To undo, move the labels back by hand.
import plistlib, sys, os
if len(sys.argv) != 3:
    sys.exit(__doc__ or 'usage: edit-s1.py VOL LIST')
vol = sys.argv[1]; lst = sys.argv[2]
want = {l.split('#')[0].strip() for l in open(lst) if l.split('#')[0].strip()}
x = os.path.join(vol, 'System/Library/xpc')
lp = os.path.join(x, 'launchd.plist'); dp = os.path.join(x, 'launchd-disabled.plist')
fmt_l = plistlib.FMT_BINARY if open(lp,'rb').read(6)==b'bplist' else plistlib.FMT_XML
fmt_d = plistlib.FMT_BINARY if open(dp,'rb').read(6)==b'bplist' else plistlib.FMT_XML
d = plistlib.load(open(lp, 'rb')); dis = plistlib.load(open(dp, 'rb'))
ld = d['LaunchDaemons']; moved = []
for k in list(ld):
    lab = ld[k].get('Label', '')
    if lab in want:
        dis[k] = ld.pop(k); moved.append(lab)
missing = sorted(want - set(moved) - {v.get('Label') for v in dis.values()})
with open(lp, 'r+b') as f:
    b = plistlib.dumps(d, fmt=fmt_l); f.seek(0); f.write(b); f.truncate()
with open(dp, 'r+b') as f:
    b = plistlib.dumps(dis, fmt=fmt_d); f.seek(0); f.write(b); f.truncate()
print(f'moved {len(moved)} jobs; now {len(ld)} daemons, {len(dis)} disabled; not found: {missing}')
mc = os.path.join(vol, 'private/var/local/mtl2-clients')
if not os.path.exists(mc):
    sys.exit(0)
s = open(mc).read().split()
if 'backboardd' in s:
    s.remove('backboardd')
    with open(mc, 'r+') as f:
        t = ' '.join(s) + '\n'; f.seek(0); f.write(t); f.truncate()
print('mtl2-clients:', open(mc).read().strip())
