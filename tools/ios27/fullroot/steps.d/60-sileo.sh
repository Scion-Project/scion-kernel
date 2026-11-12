#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 60-sileo.sh ROOT [undo] : the package manager on an iOS 27 root, as the 16.5 line has it by default: the Procursus
# bootstrap with Sileo in it (tools/mk-jb.sh: both fetched from apt.procurs.us, both pinned by SHA-256), rootless, all of
# it under /var/jb. Default ON (ENABLE_SILEO=0 leaves the root without it). Idempotent. What it puts on the root:
#   /jb                          the prefix as the image carries it (read, never run from): jb_seed copies it to
#                                /private/var/jb at the first boot and leaves that copy alone afterwards -- what dpkg and
#                                apt install there is the phone's own. A different stamp (/jb/.scion-stamp: REV below and
#                                mk-jb.sh's) moves the phone's copy aside and seeds this one.
#   /usr/local/bin/jb_seed       userland/src/jb_seed.c, the 16.5 line's, built for iOS (tools/ios27/sileo/build-tools.sh)
#   /usr/local/bin/var_sync      userland/src/var_sync.c: the volume to the flash every 5 s (the phone is reset, never
#                                shut down; without it what was installed in the last seconds is gone)
#   /bin/{chmod,cp,mkdir,mv,rm}  links into the prefix: Sileo runs them by these names and iOS's /bin has df and ps only
#   two launchd jobs             com.scion.jbseed (once a boot), com.scion.varsync (stays), in /System/Library/xpc/launchd.plist
# What it does NOT put there, checked here so that a prefix made with other settings stops the build: no OpenSSH, no
# tweak injection (ElleKit), no repository but Procursus (and the project's own, if SCION_REPO_URL names where it is
# hosted), no account with a password, no file that everybody may write, no set-user-ID program beyond the bootstrap's
# own and Sileo's helper.
# Owners: run as root on a mount with owners (the full build), everything is root:wheel here. Run without root (an
# edit of a pulled root, owners off), the files carry the builder's user ID and jb_seed makes them root's on the phone
# before anything else (claim() there); the copy in /private/var/jb is root:wheel either way.
# SILEO_SELFTEST=1 (a test root, never a release): tools/ios27/sileo/selftest.sh as a third job, which says into
# /private/var/scion-test/selftest.log at every boot what the prefix is (owners, dpkg, Sileo registered, a kept file);
# with SELFTEST_REPO=DIR, a flat apt repository with a test.conf, it also installs, runs and removes a package from it.
#   undo: the four things above taken off the root again. /private/var/jb, the phone's own copy with what was installed
#   into it, stays unless PURGE_PREFIX=1.
set -euo pipefail
R=${1:?usage: 60-sileo.sh ROOT [undo]}; DO=${2:-apply}
H=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$H/../../../.." && pwd)
JB=${JB_ROOT:-$REPO/jb}
REV=1                                   # of what this step makes of the prefix: raise it and phones seed again
BIN_NAMES="chmod cp mkdir mv rm"
SETID="Applications/Sileo.app/giveMeRoot usr/bin/chpass usr/bin/login usr/bin/newgrp usr/bin/passwd usr/bin/quota usr/bin/su usr/bin/sudo"
die() { echo "60-sileo: $*" >&2; exit 1; }
[[ -f $R/System/Library/xpc/launchd.plist && -d $R/private/var ]] || die "$R is not an iOS root"

if [[ $DO == undo ]]; then
	/usr/bin/python3 "$H/../../sileo/jobs.py" "$R" remove
	for c in $BIN_NAMES; do
		[[ -L $R/bin/$c && $(readlink "$R/bin/$c") == /var/jb/usr/bin/$c ]] && rm -f "$R/bin/$c"
	done
	rm -f "$R/usr/local/bin/jb_seed" "$R/usr/local/bin/var_sync" "$R/usr/local/libexec/scion-sileo-selftest.sh"
	rmdir "$R/usr/local/libexec" 2>/dev/null || true
	rm -rf "$R/jb" "$R/jb.new"
	[[ ${PURGE_PREFIX:-0} == 1 ]] && rm -rf "$R/private/var/jb" "$R/private/var/.jb-seeding" "$R/private/var/log/scion-jbseed.log" "$R/private/var/log/scion-varsync.log" "$R/private/var/scion-test"
	echo "60-sileo: taken off the root$([[ ${PURGE_PREFIX:-0} == 1 ]] && echo ', the phone'"'"'s copy too' || echo '; /private/var/jb, if the phone has made one, stays')"
	exit 0
fi
[[ $DO == apply ]] || die "what is '$DO'? (nothing, or undo)"
[[ ${ENABLE_SILEO:-1} == 1 ]] || { echo "60-sileo: off (ENABLE_SILEO=0)"; exit 0; }

# the inputs, made before the build by tools/ios27/sileo/prepare.sh (no root, network for the two downloads)
P=$JB/root/var/jb
[[ -x $P/Applications/Sileo.app/Sileo && -s $JB/.stamp ]] || die "no prefix at $P: run tools/ios27/sileo/prepare.sh"
[[ $(cat "$JB/.stamp") == *-- ]] || die "the prefix at $P was made with PREINSTALL or ElleKit: run tools/ios27/sileo/prepare.sh (it makes the plain one)"
(cd "$JB/ios27" 2>/dev/null && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) || die "no jb_seed and var_sync at $JB/ios27: run tools/ios27/sileo/prepare.sh"
for c in $BIN_NAMES; do
	[[ -x $P/usr/bin/$c ]] || die "the prefix has no usr/bin/$c"
	if [[ -e $R/bin/$c || -L $R/bin/$c ]]; then
		[[ -L $R/bin/$c && $(readlink "$R/bin/$c") == /var/jb/usr/bin/$c ]] || die "/bin/$c is on the root already and is not this step's link"
	fi
done

# the image's copy, made aside and put in place whole
rm -rf "$R/jb.new"
cp -a "$P" "$R/jb.new"
N=$R/jb.new
# the repositories Sileo starts with: Procursus, where all of it came from; the project's own once it is hosted
find "$N/etc/apt/sources.list.d" -type f ! -name procursus.sources -delete
if [[ -n ${SCION_REPO_URL:-} ]]; then
	[[ $SCION_REPO_URL == https://* ]] || die "SCION_REPO_URL must be https://"
	printf 'Types: deb\nURIs: %s\nSuites: ./\nComponents:\n' "$SCION_REPO_URL" > "$N/etc/apt/sources.list.d/scion.sources"
fi
# no account with a password: the bootstrap ships one for mobile (its own first-run script asks for a new one, and that
# script is not used here). "!" matches nothing; pwd_mkdb makes the databases from this file on the phone (prep.sh).
awk -F: -v OFS=: '/^#/ || NF < 10 { print; next } $2 != "*" && $2 != "!" { $2 = "!"; n++ } { print } END { printf "60-sileo: %d password(s) taken out of master.passwd\n", n > "/dev/stderr" }' \
	"$N/etc/master.passwd" > "$N/etc/master.passwd.new"
cat "$N/etc/master.passwd.new" > "$N/etc/master.passwd" && rm -f "$N/etc/master.passwd.new"
rm -f "$N/etc/pwd.db" "$N/etc/spwd.db"
# what mk-jb.sh takes out of dpkg's database and leaves on the disk (a set-user-ID daemon of a jailbreak's kernel
# access, and its job): nothing here has a use for it
rm -f "$N/usr/sbin/shshd" "$N/Library/LaunchDaemons/us.diatr.shshd.plist"
printf 'ios27-r%s-%s' "$REV" "$(cat "$JB/.stamp")" > "$N/.scion-stamp"

# the checks: what must not be there
for f in usr/sbin/sshd usr/bin/ssh usr/libexec/sshd-keygen-wrapper usr/lib/TweakLoader.dylib usr/lib/libellekit.dylib usr/sbin/dropbear; do
	[[ ! -e $N/$f ]] || die "$f is in the prefix: this root is to carry neither OpenSSH nor tweak injection"
done
[[ -z $(awk -F: '!/^#/ && NF >= 10 && $2 != "*" && $2 != "!"' "$N/etc/master.passwd") ]] || die "an account with a password is left in master.passwd"
W=$(cd "$N" && find . ! -type l -perm -0002 | head -5)
[[ -z $W ]] || die "writable by everybody in the prefix: $W"
HAVE=$(cd "$N" && find . -type f -perm +6000 | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')
WANT=$(printf '%s\n' $SETID | LC_ALL=C sort | tr '\n' ' ')
[[ $HAVE == "$WANT" ]] || die "the set-ID programs are not the expected ones: have: $HAVE want: $WANT"
# every Mach-O plain arm64: an arm64e slice would need the PAC rewrite the rest of the root went through (10-macho-a64e
# runs before this step and leaves /jb alone), and a fat file is not what the bootstrap ships
/usr/bin/python3 - "$N" <<'PY' || die "the prefix has Mach-O files that are not plain arm64 (listed above)"
import os, struct, sys
bad = n = 0
for d, _, files in os.walk(sys.argv[1]):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as h:
            head = h.read(12)
        if len(head) < 12:
            continue
        magic, cpu, sub = struct.unpack('<III', head)
        if magic in (0xbebafeca, 0xbfbafeca):                   # fat, as read little-endian
            print('60-sileo: fat Mach-O:', p, file=sys.stderr); bad += 1
        elif magic == 0xfeedfacf:
            n += 1
            if cpu != 0x0100000c or (sub & 0xffffff) != 0:
                print('60-sileo: not plain arm64 (cpu %#x subtype %#x): %s' % (cpu, sub, p), file=sys.stderr); bad += 1
print('60-sileo: %d Mach-O files in the prefix, all plain arm64' % n if not bad else '60-sileo: %d Mach-O files are not plain arm64' % bad)
sys.exit(1 if bad else 0)
PY
[[ $(find "$N/etc/apt/sources.list.d" -type f | wc -l | tr -d ' ') == $([[ -n ${SCION_REPO_URL:-} ]] && echo 2 || echo 1) ]] || die "the list of repositories is not what was written"

if [[ $EUID == 0 ]]; then
	chown -R 0:0 "$N"
	chown 501:501 "$N/var/mobile"
	for f in $SETID; do                     # chown took the bits off
		case $f in usr/bin/quota) chmod 2755 "$N/$f" ;; *) chmod 4755 "$N/$f" ;; esac
	done
fi
rm -rf "$R/jb"
mv "$N" "$R/jb"

install -d -m 0755 "$R/usr/local/bin"
for t in jb_seed var_sync; do
	cat "$JB/ios27/$t" > "$R/usr/local/bin/$t.new.$$" && chmod 0755 "$R/usr/local/bin/$t.new.$$" && mv -f "$R/usr/local/bin/$t.new.$$" "$R/usr/local/bin/$t"
done
for c in $BIN_NAMES; do
	ln -sfn "/var/jb/usr/bin/$c" "$R/bin/$c"
done
if [[ $EUID == 0 ]]; then
	chown 0:0 "$R/usr/local/bin/jb_seed" "$R/usr/local/bin/var_sync"
	for c in $BIN_NAMES; do chown -h 0:0 "$R/bin/$c"; done
fi
if [[ ${SILEO_SELFTEST:-0} == 1 ]]; then
	install -d -m 0755 "$R/usr/local/libexec"
	cat "$H/../../sileo/selftest.sh" > "$R/usr/local/libexec/scion-sileo-selftest.sh" && chmod 0755 "$R/usr/local/libexec/scion-sileo-selftest.sh"
	[[ $EUID != 0 ]] || chown 0:0 "$R/usr/local/libexec" "$R/usr/local/libexec/scion-sileo-selftest.sh"
	if [[ -n ${SELFTEST_REPO:-} ]]; then
		[[ -f $SELFTEST_REPO/Packages && -f $SELFTEST_REPO/test.conf ]] || die "SELFTEST_REPO=$SELFTEST_REPO has no Packages and test.conf"
		rm -rf "$R/private/var/scion-test/repo"
		install -d -m 0755 "$R/private/var/scion-test"
		cp -R "$SELFTEST_REPO" "$R/private/var/scion-test/repo"
	fi
	/usr/bin/python3 "$H/../../sileo/jobs.py" "$R" add --selftest
	echo "60-sileo: SILEO_SELFTEST=1: this is a test root (a third job, com.scion.sileotest)"
else
	rm -f "$R/usr/local/libexec/scion-sileo-selftest.sh"
	/usr/bin/python3 "$H/../../sileo/jobs.py" "$R" add
fi
echo "60-sileo: /jb ($(du -sh "$R/jb" | cut -f1), stamp $(cut -c1-24 "$R/jb/.scion-stamp")), jb_seed, var_sync, /bin links, 2 launchd jobs$([[ $EUID == 0 ]] || echo '; no root here: jb_seed makes the owners right on the phone')"
