#!/bin/bash
# 70-ui-stability.sh ROOT [undo] : iOS 27 on the Galaxy S9, user-space stability fixes found on the night of 13-14 Nov 2026.
# SPDX-License-Identifier: MIT-0
#
# 1. atc (AirTrafficControl) aborted 12-17 times every boot with Wi-Fi up: MusicLibrary throws when the media library's
#    directory is missing (/private/var/mobile/Media/iTunes_Control, made by a restore on a real phone; absent on this
#    root). Made here, owned by mobile, with its iTunes/ and Music/ subdirectories.
# 2. com.scion.lasttime: the phone has no clock that iOS reads at start-up, so every boot began on 1 January 1970 until
#    the network set the time. The job keeps the time in /private/var/db/scion-lasttime (every 5 minutes, and at the
#    end of a clean shutdown it is at most 5 minutes old) and, once a boot, puts the clock forward to it when the clock
#    is behind. Nothing is put back; timed's network time wins as soon as it comes.
# Undo removes the job and its script; the directories of 1 stay (harmless, and the media library may have filled them).
set -euo pipefail
R=$1
J=$R/System/Library/xpc/launchd.plist
[[ -f $J ]] || { echo "70-ui-stability: $R has no launchd.plist" >&2; exit 1; }
S=$R/usr/local/libexec/scion-lasttime.sh
if [[ ${2:-} == undo ]]; then
	rm -f "$S"
else
	M=$R/private/var/mobile/Media/iTunes_Control
	mkdir -p "$M/iTunes" "$M/Music"
	chmod 0755 "$M" "$M/iTunes" "$M/Music"
	[[ $EUID != 0 ]] || chown -R 501:501 "$M"
	mkdir -p "$R/usr/local/libexec"
	cat > "$S" <<'SH'
#!/var/jb/bin/bash
# scion-lasttime (tools/ios27/ui/70-ui-stability.sh): the last known time across boots (the S9 gives iOS no clock at start-up)
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
F=/private/var/db/scion-lasttime
L=/private/var/log/scion-lasttime.log
now=$(date -u +%s)
saved=$(cat "$F" 2>/dev/null || echo 0)
case $saved in ''|*[!0-9]*) saved=0;; esac
if [ "$saved" -gt "$now" ]; then
	date -u -s "@$saved" > /dev/null 2>&1 || date -u -f %s "$saved" > /dev/null 2>&1   # GNU coreutils (the prefix), else BSD
	echo "$(date -u +%FT%TZ) clock was $now, put forward to the saved $saved" >> "$L"
else
	echo "$(date -u +%FT%TZ) clock $now not behind the saved $saved" >> "$L"
fi
while true; do
	date -u +%s > "$F.new" && mv -f "$F.new" "$F"
	sleep 300
done
SH
	chmod 0755 "$S"
	[[ $EUID != 0 ]] || chown 0:0 "$S"
fi
/usr/bin/python3 - "$J" "${2:-add}" <<'PY'
import plistlib, sys
path, how = sys.argv[1:]
key = '/System/Library/LaunchDaemons/com.scion.lasttime.plist'
d = plistlib.load(open(path, 'rb')); jobs = d['LaunchDaemons']; before = key in jobs
if how == 'undo':
    jobs.pop(key, None)
else:
    jobs[key] = {'Label': 'com.scion.lasttime', 'ProgramArguments': ['/var/jb/bin/bash', '/usr/local/libexec/scion-lasttime.sh'],
                 'KeepAlive': {'PathState': {'/private/var/jb/.prepared': True}},
                 'LimitLoadToSessionType': 'System', 'ProcessType': 'Background', 'UserName': 'root'}
data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY); plistlib.loads(data)
with open(path, 'r+b') as f:
    f.truncate(0); f.write(data)
print('70-ui-stability: com.scion.lasttime %s (was %s)' % ('removed' if how == 'undo' else 'added', 'there' if before else 'absent'))
PY
