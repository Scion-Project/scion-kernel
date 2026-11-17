#!/bin/bash
# 66-celld.sh ROOT BIN [undo|test|untest] : the modem's data context as an iOS network service. SPDX-License-Identifier: MIT-0
#
# Installs scion-celld (BIN/scion-celld, from build.sh) as /usr/local/bin/scion-celld and its job com.scion.celld in iOS's
# launchd cache (/System/Library/xpc/launchd.plist): run at load as root; it exits 0 at once on a boot without the modem
# (no cp=) and launchd leaves it; it is started again only if it fails. With the modem's cp_if=1 it publishes pdp_ip0 to
# configd (see scion-celld.c), so names resolve and iOS routes over cellular when nothing better is up.
#   test    also scion-fetch and the TEST job com.scion.celltest (never on a release root): once a boot it waits up to 15 min
#           for the context, then into /private/var/log/scion-celltest.log (and the console) configd's view, the routes,
#           and one https GET by name (CELLTEST_URL in /private/var/scion-test/celltest-url, default https://example.com/)
#   untest  removes the test job and scion-fetch;  undo  removes everything this step added
set -euo pipefail
R=$1; B=${2:-}; how=${3:-add}
J=$R/System/Library/xpc/launchd.plist
[[ -f $J ]] || { echo "66-celld: $R has no launchd.plist" >&2; exit 1; }
T=$R/usr/local/libexec/scion-celltest.sh
case $how in
add|test)
	install -d "$R/usr/local/bin" "$R/usr/local/libexec"
	install -m 0755 "$B/scion-celld" "$R/usr/local/bin/scion-celld"
	# the status bar's library (c.dylib, scion-bars.m): loaded only through the boot-arg sweetinject (/usr/lib/c.dylib,SpringBoard)
	# and on only while /private/var/local/scion-bars exists
	if [[ -f $B/c.dylib ]]; then
		install -m 0755 "$B/c.dylib" "$R/usr/lib/c.dylib"
		mkdir -p "$R/private/var/local"
		[[ -f $R/private/var/local/scion-bars ]] || printf 'mode=hook\n' > "$R/private/var/local/scion-bars"
		[[ $how != test ]] || grep -q '^discover=' "$R/private/var/local/scion-bars" || echo discover=1 >> "$R/private/var/local/scion-bars"
	fi
	if [[ $how == test ]]; then
		install -m 0755 "$B/scion-fetch" "$R/usr/local/bin/scion-fetch"
		mkdir -p "$R/private/var/scion-test"
		cat > "$T" <<'SH'
#!/var/jb/bin/bash
# scion-celltest (tools/ios27/modem/66-celld.sh test): a test root's cellular check, once a boot
export PATH=/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
L=/private/var/log/scion-celltest.log
say() { echo "$*" >> $L; echo "scion-celltest: $*" > /dev/console 2>/dev/null || true; }
U=$(cat /private/var/scion-test/celltest-url 2>/dev/null || echo https://example.com/)
say "=== $(date -u +%FT%TZ) celltest, url $U"
for i in $(seq 1 180); do
	info=$(sysctl -n kern.hakios.cp.info 2>/dev/null || true)
	case "$info" in *" data=1 "*) break;; esac
	case "$info" in *"stage=0 "*|"") say "no modem in this boot: $info"; exit 0;; esac
	sleep 5
done
say "after $((i * 5)) s: $info"
sleep 10
scion-celld -d 2>&1 | grep -v '^kern.hakios.cp.addr' | while IFS= read -r l; do say "$l"; done
netstat -rn -f inet 2>&1 | head -12 | while IFS= read -r l; do say "$l"; done
say "default route: $(route -n get default 2>&1 | grep -E 'interface' | tr -s ' ')"
for k in 1 2; do
	scion-fetch "$U" 2>&1 | while IFS= read -r l; do say "$l"; done
	sleep 5
done
say "after: $(sysctl -n kern.hakios.cp.info 2>/dev/null)"
say "=== $(date -u +%FT%TZ) celltest done"
SH
		chmod 0755 "$T"
	fi
	[[ $EUID != 0 ]] || chown 0:0 "$R/usr/local/bin/scion-celld" "$R/usr/local/bin/scion-fetch" "$T" 2>/dev/null || true
	;;
untest)
	rm -f "$T" "$R/usr/local/bin/scion-fetch";;
undo)
	rm -f "$T" "$R/usr/local/bin/scion-fetch" "$R/usr/local/bin/scion-celld" "$R/usr/lib/c.dylib" "$R/private/var/local/scion-bars" \
		"$R/private/var/local/scion-bars.boots";;
*) echo "66-celld: add, test, untest or undo" >&2; exit 2;;
esac
/usr/bin/python3 - "$J" "$how" <<'PY'
import plistlib, sys
path, how = sys.argv[1:]
d = plistlib.load(open(path, 'rb')); jobs = d['LaunchDaemons']
K = '/System/Library/LaunchDaemons/com.scion.celld.plist'
KT = '/System/Library/LaunchDaemons/com.scion.celltest.plist'
if how in ('add', 'test'):
    jobs[K] = {'Label': 'com.scion.celld', 'ProgramArguments': ['/usr/local/bin/scion-celld'], 'RunAtLoad': True,
               'KeepAlive': {'SuccessfulExit': False}, 'ThrottleInterval': 30, 'UserName': 'root',
               'LimitLoadToSessionType': 'System', 'ProcessType': 'Adaptive'}
if how == 'test':
    jobs[KT] = {'Label': 'com.scion.celltest', 'ProgramArguments': ['/var/jb/bin/bash', '/usr/local/libexec/scion-celltest.sh'],
                'RunAtLoad': True, 'WatchPaths': ['/private/var/jb/.prepared'], 'LaunchOnlyOnce': True,
                'LimitLoadToSessionType': 'System', 'ProcessType': 'Background'}
if how in ('untest', 'undo'):
    jobs.pop(KT, None)
if how == 'undo':
    jobs.pop(K, None)
data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY); plistlib.loads(data)
with open(path, 'r+b') as f:
    f.truncate(0); f.write(data)
print('66-celld: %s: celld %s, celltest %s' % (how, 'in' if K in jobs else 'out', 'in' if KT in jobs else 'out'))
PY
