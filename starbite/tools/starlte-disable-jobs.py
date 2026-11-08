#!/usr/bin/env python3
"""Take launchd jobs out of an attached iOS root's /System/Library/xpc/launchd.plist (launchd on
this root reads its jobs from there, not from the LaunchDaemons folder), by label.
   starlte-disable-jobs.py /Volumes/IOSROOT tools/starlte-disabled.list
The removed entries are kept in launchd-disabled.plist beside it, so they can be put back."""
import plistlib
import sys

root, listfile = sys.argv[1], sys.argv[2]
path = root + '/System/Library/xpc/launchd.plist'
keep_path = root + '/System/Library/xpc/launchd-disabled.plist'
want = {l.split('#')[0].strip() for l in open(listfile)} - {''}
d = plistlib.load(open(path, 'rb'))
try:
    kept = plistlib.load(open(keep_path, 'rb'))
except FileNotFoundError:
    kept = {}
gone = []
for key, job in list(d['LaunchDaemons'].items()):
    if job.get('Label') in want:
        kept[key] = d['LaunchDaemons'].pop(key)
        gone.append(job['Label'])
with open(path, 'r+b') as f:
    f.truncate(0)
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
with open(keep_path, 'wb') as f:
    plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)
print(f'{len(gone)} jobs taken out ({len(d["LaunchDaemons"])} left): {" ".join(sorted(gone))}')
missing = want - set(gone) - {j.get('Label') for j in kept.values()}
if missing:
    print('not on this root:', ' '.join(sorted(missing)))
