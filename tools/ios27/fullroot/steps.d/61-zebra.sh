#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 61-zebra.sh ROOT [undo] : Zebra, the other package manager, where LaunchServices registers it. Zebra (the Zebra Team's,
# GPL-3.0; Procursus's rootless build xyz.willy.zebra 1.1.36, pinned by SHA-256 in tools/mk-jb.sh; source:
# github.com/zbrateam/Zebra and the project's fork github.com/Scion-Project/Zebra) is unpacked into the prefix by
# tools/ios27/sileo/prepare.sh when PACKAGE_MANAGER is zebra (the default) or both, next to the same apt and dpkg Sileo
# uses; 60-sileo.sh puts that prefix on the root as /jb. This step makes the real copy /Applications/Zebra.app (marked
# .scion-prefix-app: tools/ios27/sileo/pm-app.sh) that lsd registers at the next boot. A separate program on the root:
# nothing of this repository links with it.
# What Zebra needs on this port and has: no set-user-ID helper (on iOS 13 and later it becomes root by a spawn into
# persona 99 with uid 0, which the kernel allows with persona_lax=1: the fork's kern_exec.c), its sources in the prefix's
# etc/apt, uikittools' uicache (its package script, not run by an unpack by hand; scion-apps-sync does the copy at boot).
# Idempotent. Nothing to do when the prefix has no Zebra (PACKAGE_MANAGER=sileo) or the root has no package manager
# (ENABLE_SILEO=0): a copy left by an earlier build is taken off then.
#   undo: the copy taken off again (the prefix, with Zebra in it, is 60-sileo.sh's to take off).
set -euo pipefail
R=${1:?usage: 61-zebra.sh ROOT [undo]}; DO=${2:-apply}
H=$(cd "$(dirname "$0")" && pwd)
. "$H/../../sileo/pm-app.sh"
die() { echo "61-zebra: $*" >&2; exit 1; }
[[ -f $R/System/Library/xpc/launchd.plist && -d $R/private/var ]] || die "$R is not an iOS root"

case $DO in
undo)
	pm_app_take "$R" Zebra.app
	echo "61-zebra: /Applications/Zebra.app taken off"
	exit 0 ;;
apply) ;;
*) die "what is '$DO'? (nothing, or undo)" ;;
esac
if [[ ${ENABLE_SILEO:-1} != 1 || ! -d $R/jb/Applications/Zebra.app ]]; then
	pm_app_take "$R" Zebra.app
	echo "61-zebra: no Zebra in the prefix (PACKAGE_MANAGER=sileo, or ENABLE_SILEO=0): nothing to do"
	exit 0
fi
Z=$R/jb/Applications/Zebra.app
[[ -x $Z/Zebra ]] || die "the prefix's Zebra.app has no executable"
S=$(cd "$Z" && find . -type f -perm +6000 | head -3)
[[ -z $S ]] || die "set-ID files in Zebra.app (none expected): $S"
pm_app_put "$R" Zebra.app || die "could not copy Zebra.app"
V=$(awk '$1 == "Package:" { x = ($2 == "xyz.willy.zebra") } x && $1 == "Version:" { print $2; exit }' "$R/jb/Library/dpkg/status")
echo "61-zebra: /Applications/Zebra.app (Zebra ${V:-?})$([[ $EUID == 0 ]] || echo '; no root here: prep.sh makes it root'"'"'s at boot')"
