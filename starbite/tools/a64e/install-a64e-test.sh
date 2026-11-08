#!/usr/bin/env bash
# Put build/a64e/a64e-test on the phone's iOS root (TWRP) as a run-once launchd job
# (com.starbite.a64e-test, at boot; output on /dev/console -> the flash log).
#   tools/a64e/install-a64e-test.sh            pull the live root first, then push
#   tools/a64e/install-a64e-test.sh --remove   take the job out again
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tools/starlte-root-sync.py pull build/root-phone.img build/root-phone-b.img | tail -1
mv build/root-phone-b.img build/root-phone.img
mv build/root-phone-b.img.hashes build/root-phone.img.hashes
hdiutil attach -owners off build/root-phone.img 2>/dev/null | tail -1
R=/Volumes/IOSROOT
python3 - "$R" "${1:-}" <<'PY'
import plistlib, sys
root, mode = sys.argv[1], sys.argv[2]
path = root + '/System/Library/xpc/launchd.plist'
key = '/System/Library/LaunchDaemons/com.starbite.a64e-test.plist'
d = plistlib.load(open(path, 'rb'))
if mode == '--remove':
    d['LaunchDaemons'].pop(key, None)
else:
    d['LaunchDaemons'][key] = {
        'Label': 'com.starbite.a64e-test',
        'ProgramArguments': ['/usr/local/starbite/bin/a64e-test'],
        'RunAtLoad': True,
        'LaunchOnlyOnce': True,
        'UserName': 'root',
    }
with open(path, 'r+b') as f:
    f.truncate(0)
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print('job', 'removed' if mode == '--remove' else 'added', '-', len(d['LaunchDaemons']), 'jobs')
PY
if [ "${1:-}" != --remove ]; then
	mkdir -p $R/usr/local/starbite/bin
	cp build/a64e/a64e-test $R/usr/local/starbite/bin/a64e-test
	chmod 755 $R/usr/local/starbite/bin/a64e-test
fi
sync
hdiutil detach $R 2>/dev/null | tail -1
python3 tools/starlte-root-sync.py push build/root-phone.img | tail -1
