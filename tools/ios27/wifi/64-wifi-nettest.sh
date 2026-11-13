#!/bin/bash
# 64-wifi-nettest.sh ROOT [undo] : a TEST root's network check at every boot (never a release root). SPDX-License-Identifier: MIT-0
#
# Adds the job com.scion.nettest to iOS's launchd cache (/System/Library/xpc/launchd.plist) and its script
# /usr/local/libexec/scion-nettest.sh. Once a boot, after the package manager's prefix is there (/private/var/jb/.prepared,
# 60-sileo.sh), it waits up to NETTEST_WAIT seconds (default 900) for an IPv4 address on any interface but lo0, then says into
# /private/var/log/scion-nettest.log: the interfaces and routes, an HTTP GET of a public page done by user space (bash's
# /dev/tcp, so the name goes through iOS's own resolver), and `apt-get update` (https, the prefix's sources). Nothing listens.
# The interface to test is named in /private/var/scion-test/nettest-if (default: whatever has an address; "pdp_ip0" for the
# modem's), so the same step tests Wi-Fi (en0) and the cellular interface.
set -euo pipefail
R=$1
J=$R/System/Library/xpc/launchd.plist
[[ -f $J ]] || { echo "64-wifi-nettest: $R has no launchd.plist" >&2; exit 1; }
S=$R/usr/local/libexec/scion-nettest.sh
if [[ ${2:-} == undo ]]; then
	rm -f "$S"
else
	mkdir -p "$R/usr/local/libexec" "$R/private/var/scion-test"
	cat > "$S" <<'SH'
#!/var/jb/bin/bash
# scion-nettest (tools/ios27/wifi/64-wifi-nettest.sh): a test root's network check, once a boot
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
L=/private/var/log/scion-nettest.log
exec >>"$L" 2>&1
W=${NETTEST_WAIT:-900}
IF=$(cat /private/var/scion-test/nettest-if 2>/dev/null || true)
echo "=== $(date -u +%FT%TZ) nettest (interface: ${IF:-any})"
addr=
for i in $(seq 1 $((W / 5))); do
	if [[ -n $IF ]]; then addr=$(/sbin/ifconfig "$IF" 2>/dev/null | grep 'inet ' | head -1)
	else addr=$(/sbin/ifconfig -a 2>/dev/null | grep 'inet ' | grep -v '127.0.0.1' | head -1); fi
	[[ -n $addr ]] && break
	sleep 5
done
echo "address after $((i * 5)) s: ${addr:-none}"
/sbin/ifconfig -a 2>&1 | grep -E '^[a-z]|inet |status' | head -40
/usr/sbin/netstat -rn -f inet 2>&1 | head -20
[[ -n $addr ]] || { echo "=== no address: no fetch"; exit 0; }
fetch() {   # fetch HOST PATH: an HTTP/1.1 GET by bash itself (name through iOS's resolver), at most 45 s
	local t0=$(date +%s) out=/tmp/scion-nettest.$$
	echo "fetch http://$1$2: start"
	timeout 45 bash -c 'exec 3<>"/dev/tcp/$1/80" || exit 7
		printf "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: scion-nettest\r\n\r\n" "$2" "$1" >&3
		head -c 4000 <&3' _ "$1" "$2" > $out
	local rc=$?
	echo "fetch http://$1$2: rc $rc, $(wc -c < $out) bytes in $(( $(date +%s) - t0 )) s, first lines:"
	head -3 $out | tr -d '\r'; grep -o '<title>[^<]*' $out | head -1
	rm -f $out
}
fetch example.com /
fetch captive.apple.com /hotspot-detect.html
if [[ ! -f /private/var/scion-test/nettest-noapt ]]; then
	echo "--- apt-get update"
	/var/jb/usr/bin/timeout 180 /var/jb/usr/bin/apt-get update 2>&1 | tail -8
fi
echo "=== $(date -u +%FT%TZ) nettest done"
SH
	chmod 0755 "$S"
	[[ $EUID != 0 ]] || chown 0:0 "$S"
fi
/usr/bin/python3 - "$J" "${2:-add}" <<'PY'
import plistlib, sys
path, how = sys.argv[1:]
key = '/System/Library/LaunchDaemons/com.scion.nettest.plist'
d = plistlib.load(open(path, 'rb')); jobs = d['LaunchDaemons']; before = key in jobs
if how == 'undo':
    jobs.pop(key, None)
else:
    jobs[key] = {'Label': 'com.scion.nettest', 'ProgramArguments': ['/var/jb/bin/bash', '/usr/local/libexec/scion-nettest.sh'],
                 'RunAtLoad': True, 'WatchPaths': ['/private/var/jb/.prepared'], 'LaunchOnlyOnce': True,
                 'LimitLoadToSessionType': 'System', 'ProcessType': 'Background'}
data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY); plistlib.loads(data)
with open(path, 'r+b') as f:
    f.truncate(0); f.write(data)
print('64-wifi-nettest: com.scion.nettest %s (was %s)' % ('removed' if how == 'undo' else 'added', 'there' if before else 'absent'))
PY
