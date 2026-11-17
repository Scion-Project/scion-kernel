#!/bin/bash
# 62-wifid-loadable.sh ROOT [undo] : let launchd start iOS's Wi-Fi daemon on the S9. Idempotent; a root-build step (needs root, ROOT
# mounted with owners on). By hand it is OPT-IN (WIFI_WIFID=1); the root build's Wi-Fi step (fullroot/steps.d/91-wifi.sh) runs it
# by default, WIFI_WIFID=0 leaving wifid out.
#
# iOS 27's com.apple.wifid job carries `_LimitLoadToDeviceTree` = (wlan, marconi-wifi, centauri-alpha, ...): launchd loads it
# only where the device tree has such a node, and the S9's ADT has none, so wifid never starts and Settings shows Wi-Fi off
# even with the radio joined and pinging in the kernel (phone 2, 10 Nov 2026). Scion's 16.5 build drops the key from the job
# (userland/mkjobs.py); this does the same in the launchd cache /System/Library/xpc/launchd.plist (and in
# launchd-disabled.plist, should the job have been moved there), and in the job's own plist under LaunchDaemons.
# The other way, without touching the root, is a test image whose ADT has /arm-io/wlan (adt-wlan-node.py): that is the way
# wifid has been seen started on a phone (13 Nov 2026); this step's way has NOT been run on a phone.
# The values taken out are kept in /usr/local/share/scion/wifid-limit.plist (written once, at the first run that takes one out).
#   undo  puts them back where they were and deletes that file.
set -euo pipefail
R=$1
if [[ ${2:-} != undo ]]; then
	[[ ${WIFI_WIFID:-0} == 1 ]] || { echo "62-wifid-loadable: skipped (WIFI_WIFID=1 enables it)"; exit 0; }
fi
[[ -f $R/System/Library/xpc/launchd.plist ]] || { echo "62-wifid-loadable: $R has no launchd.plist" >&2; exit 1; }
/usr/bin/python3 - "$R" "${2:-apply}" <<'PY'
import os, plistlib, sys
root, how = sys.argv[1:]
KEEP = os.path.join(root, 'usr/local/share/scion/wifid-limit.plist')
kept = plistlib.load(open(KEEP, 'rb')) if os.path.exists(KEEP) else {}
LABEL = 'com.apple.wifid'
changed = 0

def write(p, d, st):
    data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY); plistlib.loads(data)
    with open(p, 'r+b') as f:
        f.seek(0); f.write(data); f.truncate()
    os.chown(p, st.st_uid, st.st_gid)

def jobs_of(rel, d):
    if rel.endswith('/com.apple.wifid.plist'):
        return {'': d}
    return d.get('LaunchDaemons', d) if rel.endswith('launchd.plist') else d

for rel in ('System/Library/xpc/launchd.plist', 'System/Library/xpc/launchd-disabled.plist',
            'System/Library/LaunchDaemons/com.apple.wifid.plist'):
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        continue
    st = os.stat(p); d = plistlib.load(open(p, 'rb')); n = 0
    for k, j in jobs_of(rel, d).items():
        if not isinstance(j, dict) or j.get('Label') != LABEL:
            continue
        slot = rel + '|' + k
        if how == 'undo':
            if slot in kept and '_LimitLoadToDeviceTree' not in j:
                j['_LimitLoadToDeviceTree'] = kept[slot]; n += 1
        elif '_LimitLoadToDeviceTree' in j:
            kept.setdefault(slot, j.pop('_LimitLoadToDeviceTree')); n += 1
    if n:
        write(p, d, st); changed += n
if how == 'undo':
    if os.path.exists(KEEP):
        os.remove(KEEP)
        for dname in (os.path.dirname(KEEP), os.path.dirname(os.path.dirname(KEEP))):
            try:
                os.rmdir(dname)
            except OSError:
                pass
    print('62-wifid-loadable: undone, com.apple.wifid limited to its device-tree nodes again (%d place(s) changed)' % changed)
else:
    if changed:
        os.makedirs(os.path.dirname(KEEP), exist_ok=True)
        with open(KEEP, 'wb') as f:
            plistlib.dump(kept, f, fmt=plistlib.FMT_XML)
    print('62-wifid-loadable: com.apple.wifid no longer limited to a device-tree node (%d place(s) changed)' % changed)
PY
