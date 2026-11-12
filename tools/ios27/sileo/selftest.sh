#!/var/jb/bin/sh
# SPDX-License-Identifier: MIT-0
# selftest.sh : what the package manager's prefix is on this phone, said into a file at every boot. The iOS 27 port has
# no shell to ask, so a test root carries this as a launchd job (SILEO_SELFTEST=1 with tools/ios27/fullroot/steps.d/
# 60-sileo.sh; a release root does not). It runs as root, once a boot, after jb_seed has prepared the prefix, and
# appends to /private/var/scion-test/selftest.log (read back from the root in TWRP: tools/ios27/sileo/README.md).
#   every boot   owners and modes, nothing writable by everybody, the set-ID programs, no account with a password,
#                no OpenSSH, dpkg's database whole, Sileo registered, the repositories, the boot counter and the
#                kept file (one line a boot in mobile's Documents: persistence), the network as it stands
#   with a repository in /private/var/scion-test/repo (tools/ios27/sileo/mk-test-repo.py; test.conf names PKG, RUN,
#   APP and APPID): the first boot installs PKG with apt-get from it, runs it, removes it, installs it again and
#   installs APP; every later boot wants both still installed, PKG still running and APP still registered.
# A line starts with "ok", "FAIL" or "info"; the last line of a boot is "RESULT boot N: PASS" or "... FAIL (n)".
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
HOME=/var/root
export PATH HOME
T=/private/var/scion-test
mkdir -p $T
exec >> $T/selftest.log 2>&1
# a prefix from an earlier boot: this boot's jb_seed (owners, prep.sh) gets its time first
[ ! -e /var/jb/.prepared ] || sleep 45
n=0
while [ ! -e /var/jb/.prepared ] && [ $n -lt 120 ]; do sleep 5; n=$((n + 1)); done
B=$(($(cat $T/boots 2>/dev/null || echo 0) + 1))
echo $B > $T/boots
F=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; F=$((F + 1)); }
info() { echo "info $*"; }
want() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: $2 (wanted $3)"; }
echo "=== selftest boot $B, $(date -u +%Y-%m-%dT%H:%M:%SZ), waited $((n * 5)) s for the prefix"
[ -e /var/jb/.prepared ] || { bad "no prepared prefix at /var/jb after 600 s"; echo "RESULT boot $B: FAIL ($F)"; exit 1; }
info "$(uname -srm | cut -c1-90)"
info "stamp $(cut -c1-40 /var/jb/.scion-stamp 2>/dev/null)"

# owners and modes
want "/var/jb" "$(stat -c '%u:%g %a' /private/var/jb)" "0:0 755"
want "giveMeRoot" "$(stat -c '%u:%g %a' /var/jb/Applications/Sileo.app/giveMeRoot)" "0:0 4755"
want "/var/jb/var/mobile" "$(stat -c '%u:%g' /var/jb/var/mobile)" "501:501"
for f in /jb /usr/local/bin/jb_seed /usr/local/bin/var_sync /bin/cp /var/jb/usr/bin/dpkg /var/jb/etc/master.passwd; do
	want "owner of $f" "$(stat -c '%u:%g' $f)" "0:0"
done
want "not root's outside var/ in the prefix and in /jb" "$(find /var/jb/usr /var/jb/bin /var/jb/sbin /var/jb/Applications /var/jb/Library /var/jb/etc /jb ! -user 0 2>/dev/null | wc -l | tr -d ' ')" "0"
want "writable by everybody in the prefix" "$(find /private/var/jb ! -type l -perm -0002 2>/dev/null | wc -l | tr -d ' ')" "0"
info "set-ID programs: $(find /private/var/jb -type f -perm /6000 2>/dev/null | sed 's|/private/var/jb/||' | sort | tr '\n' ' ')"
want "accounts with a password" "$(awk -F: '!/^#/ && NF >= 10 && $2 != "*" && $2 != "!"' /var/jb/etc/master.passwd | wc -l | tr -d ' ')" "0"
[ ! -e /var/jb/usr/sbin/sshd ] && ok "no OpenSSH server in the prefix" || bad "an OpenSSH server is in the prefix"
if command -v netstat >/dev/null 2>&1; then
	info "listening TCP sockets: $(netstat -an -p tcp 2>/dev/null | grep -c LISTEN) ($(netstat -an -p tcp 2>/dev/null | grep LISTEN | awk '{print $4}' | tr '\n' ' '))"
else
	info "no netstat on this root: listening sockets not listed"
fi

# dpkg and Sileo
want "dpkg --audit" "$(dpkg --audit 2>&1 | wc -l | tr -d ' ')" "0"
want "Sileo in dpkg" "$(dpkg-query -W -f='${Status}' org.coolstar.sileo 2>&1)" "install ok installed"
info "packages: $(dpkg-query -W 2>/dev/null | wc -l | tr -d ' '), firmware $(dpkg-query -W -f='${Version}' firmware 2>&1)"
uicache -l 2>/dev/null | grep -q org.coolstar.sileo && ok "Sileo registered with LaunchServices" || bad "Sileo is not registered (uicache -l)"
info "repositories: $(ls /var/jb/etc/apt/sources.list.d | tr '\n' ' ')"

# what was written before is read after
K=/private/var/mobile/Documents/scion-persist.txt
echo "boot $B $(date -u +%H:%M:%SZ)" >> $K
chown 501:501 $K
want "lines in the kept file" "$(wc -l < $K | tr -d ' ')" "$B"

# install, run, remove, from a repository on the phone itself
if [ -f $T/repo/Packages ] && [ -f $T/repo/test.conf ]; then
	. $T/repo/test.conf
	S=/var/jb/etc/apt/sources.list.d/scion-test.sources
	only="-o Dir::Etc::sourcelist=$S -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0"
	inst() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null; }
	if [ ! -e $T/installed ]; then
		printf 'Types: deb\nURIs: file:%s/repo\nSuites: ./\nComponents:\nTrusted: yes\n' $T > $S
		apt-get update $only > $T/apt-update.log 2>&1 && ok "apt-get update from $T/repo" || bad "apt-get update (see apt-update.log): $(tail -2 $T/apt-update.log | tr '\n' ' ')"
		apt-get install -y $only "$PKG" > $T/apt-install.log 2>&1
		want "$PKG after apt-get install" "$(inst "$PKG")" "install ok installed"
		$RUN > $T/run.out 2>&1 && ok "ran: $RUN: $(head -2 $T/run.out | tr '\n' ' ')" || bad "$RUN: $(head -2 $T/run.out | tr '\n' ' ')"
		apt-get remove -y "$PKG" > $T/apt-remove.log 2>&1
		case "$(inst "$PKG")" in "install ok installed") bad "$PKG still installed after apt-get remove" ;; *) ok "$PKG removed" ;; esac
		apt-get install -y $only "$PKG" >> $T/apt-install.log 2>&1
		want "$PKG installed again, to be kept" "$(inst "$PKG")" "install ok installed"
		if [ -n "${APP:-}" ]; then
			apt-get install -y $only "$APP" > $T/apt-app.log 2>&1
			want "$APP after apt-get install" "$(inst "$APP")" "install ok installed"
		fi
		: > $T/installed
		sync
	else
		want "$PKG kept across the boot" "$(inst "$PKG")" "install ok installed"
		$RUN > $T/run.out 2>&1 && ok "ran: $RUN" || bad "$RUN: $(head -2 $T/run.out | tr '\n' ' ')"
		[ -z "${APP:-}" ] || want "$APP kept across the boot" "$(inst "$APP")" "install ok installed"
	fi
	[ -f $S ] && ok "the added repository is still listed" || bad "the added repository is gone"
	if [ -n "${APPID:-}" ]; then
		sleep 10
		uicache -l 2>/dev/null | grep -q "$APPID" && ok "$APPID registered with LaunchServices" || bad "$APPID is not registered"
	fi
else
	info "no repository in $T/repo: the install test is not run"
fi

# the network, as it stands (nothing here needs it)
info "interfaces with an address: $(ifconfig -a 2>/dev/null | awk '/^[a-z]/ { i = $1 } /inet / && $2 != "127.0.0.1" { printf "%s %s ", i, $2 }')"
if apt-get update -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15 -o Acquire::Retries=0 -o Dir::Etc::sourcelist=/var/jb/etc/apt/sources.list.d/procursus.sources -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 > $T/apt-net.log 2>&1 &&
	! grep -q '^W:\|^E:\|^Err' $T/apt-net.log; then
	info "network: apt-get update from apt.procurs.us worked"
else
	info "network: apt.procurs.us not reached ($(grep -m1 '^W:\|^E:\|^Err' $T/apt-net.log | cut -c1-120))"
fi
[ $F = 0 ] && echo "RESULT boot $B: PASS" || echo "RESULT boot $B: FAIL ($F)"
sync
