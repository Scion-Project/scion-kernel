#!/bin/bash
# SPDX-License-Identifier: MIT-0
# devsh-step.sh ROOT [undo] : a TEST root only. Puts devshd (the user-space half of the console's "sh", kernel boot-arg devsh=1) on an
# attached iOS 27 root as /usr/local/bin/scion-devshd with the launchd job com.scion.devsh (an entry of /System/Library/xpc/launchd.plist).
# Without devsh=1 on the kernel's line the helper exits at once and stays off. Nothing listens on any network.
# DEVSHD=path (default: jb/ios27/devshd of the checkout, built by build.sh). undo takes both off again.
set -euo pipefail
R=${1:?usage: devsh-step.sh ROOT [undo]}
HERE=$(cd "$(dirname "$0")" && pwd)
DEVSHD=${DEVSHD:-$HERE/../../../jb/ios27/devshd}
python3 -I - "$R" "${2:-add}" <<'PY'
import plistlib, sys
root, op = sys.argv[1:]
path = root + '/System/Library/xpc/launchd.plist'
key = '/System/Library/LaunchDaemons/com.scion.devsh.plist'
job = {'Label': 'com.scion.devsh', 'ProgramArguments': ['/usr/local/bin/scion-devshd'], 'RunAtLoad': True,
       'KeepAlive': {'SuccessfulExit': False}, 'LimitLoadToSessionType': 'System', 'ProcessType': 'Interactive',
       'StandardOutPath': '/private/var/log/scion-devsh.log', 'StandardErrorPath': '/private/var/log/scion-devsh.log'}
d = plistlib.load(open(path, 'rb'))
jobs = d['LaunchDaemons']
before = dict(jobs)
if op == 'undo':
    jobs.pop(key, None)
else:
    jobs[key] = job
if jobs != before:
    data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY)
    with open(path, 'r+b') as f:
        f.truncate(0); f.write(data)
print('devsh-step: launchd job', 'removed' if op == 'undo' else 'added', '(changed)' if jobs != before else '(no change)')
PY
if [ "${2:-}" = undo ]; then
	rm -f "$R/usr/local/bin/scion-devshd"
else
	mkdir -p "$R/usr/local/bin"
	cp "$DEVSHD" "$R/usr/local/bin/scion-devshd"
	chmod 755 "$R/usr/local/bin/scion-devshd"
fi
echo "devsh-step: done (${2:-add})"
