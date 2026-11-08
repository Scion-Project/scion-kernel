#!/usr/bin/env bash
# S9-specific root tweaks on build/root.img (raw HFSX volume; IMG= another), after
# tools/build-ios-root.sh. Run as root:  sudo tools/ios27/starlte-tune-ios-root.sh
#   - drop launchd jobs for hardware the S9 port does not drive yet: they
#     only crash-loop and cost the single CPU and many UFS reads
#     (boot logs: bluetoothd x50, fairplayd.H2 x36 relaunches). iPadOS 18:
#     replicatord, cryptexd and gamed hit assertions on every start.
#   - iPadOS 18 with INSTALL_STAGED_APPS: delete the staged copies of apps
#     that are installed in /Applications (647 MB of duplicates).
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root: sudo $0" >&2; exit 2; }
cd "$(dirname "$0")/../.."
img=${IMG:-build/root.img}
[[ -f $img ]] || { echo "missing $img" >&2; exit 1; }

dev=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$img" | awk 'NR==1 {print $1}')
mnt=$(mktemp -d /tmp/iosroot.XXXX)
trap 'umount "$mnt" 2>/dev/null || true; hdiutil detach "$dev" >/dev/null 2>&1 || true; rmdir "$mnt"' EXIT
mount -t hfs -o owners "$dev" "$mnt"

launchd="$mnt/System/Library/xpc/launchd.plist"
/usr/bin/python3 - "$launchd" <<'EOF'
import plistlib, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    data = plistlib.load(f)
drop = ('bluetoothd', 'fairplayd', 'replicatord', 'security.cryptexd', 'com.apple.gamed')
removed = []
for section in ('LaunchDaemons', 'Agents', 'LaunchAgents'):
    jobs = data.get(section, {})
    for key in list(jobs):
        label = jobs[key].get('Label', '') if isinstance(jobs[key], dict) else ''
        if any(d in key or d in label for d in drop):
            removed.append(label or key)
            del jobs[key]
if not removed:
    print('launchd.plist: nothing to remove (already tuned?)')
else:
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    print('launchd.plist: removed', ', '.join(removed))
EOF
staged="$mnt/private/var/staged_system_apps"
n=0
for app in "$staged"/*.app; do
    [[ -d $app && -d $mnt/Applications/$(basename "$app") ]] || continue
    rm -rf "$app"; n=$((n + 1))
done
echo "staged_system_apps: removed $n copies already in /Applications"
sync
echo "starlte root tuning done"
