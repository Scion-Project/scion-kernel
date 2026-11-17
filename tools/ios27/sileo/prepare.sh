#!/bin/bash
# SPDX-License-Identifier: MIT-0
# prepare.sh : the inputs of tools/ios27/fullroot/steps.d/60-sileo.sh and 61-zebra.sh, made before the root build (no
# root; network for the downloads, once): the Procursus bootstrap and the package manager(s), all pinned by SHA-256 in
# tools/mk-jb.sh (PREINSTALL= ELLEKIT=: no OpenSSH, no tweak injection, nothing "newest"), and jb_seed and var_sync for iOS.
# Needs zstd, python3, Xcode's iPhoneOS SDK. Everything goes to jb/ (JB_ROOT), which is never in the repository.
#   PACKAGE_MANAGER=zebra (default, the owner's choice of 17 Nov 2026) | sileo | both: which package managers the prefix
#     has; both drive the same apt and dpkg in /var/jb. The root build takes whatever the prefix has. (ENABLE_SILEO=0 at
#     prepare-root-inputs.sh and at the build still means no package manager and no prefix at all.)
#   SILEO_FROM=fork (with sileo or both): the prefix's Sileo.app is replaced by one built from the project's fork at a
#     pinned commit (build-sileo.sh: Xcode and the network, about ten minutes the first time); the Procursus package stays
#     in dpkg's database. Default SILEO_FROM=procursus: the package's own app.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
JB=${JB_ROOT:-$REPO/jb}
die() { echo "prepare.sh: $*" >&2; exit 1; }
command -v zstd >/dev/null || die "zstd is needed (brew install zstd)"
case ${PACKAGE_MANAGER:-zebra} in
zebra) MANAGERS=zebra ;;
sileo) MANAGERS=sileo ;;
both) MANAGERS="sileo zebra" ;;
*) die "PACKAGE_MANAGER is zebra, sileo or both, not '$PACKAGE_MANAGER'" ;;
esac
# a prefix whose Sileo.app was replaced by the fork's is unpacked afresh when the other is asked for (mk-jb.sh would
# otherwise call it up to date)
[[ $(cat "$JB/.sileo-from" 2>/dev/null || echo procursus) == "${SILEO_FROM:-procursus}" ]] || rm -f "$JB/.stamp"
PREINSTALL= ELLEKIT= MANAGERS="$MANAGERS" "$REPO/tools/mk-jb.sh"
rm -f "$JB/.sileo-from"
"$REPO/tools/ios27/sileo/build-tools.sh"
case ${SILEO_FROM:-procursus} in
procursus) ;;
fork)
	[[ " $MANAGERS " == *" sileo "* ]] || die "SILEO_FROM=fork needs PACKAGE_MANAGER=sileo or both"
	"$REPO/tools/ios27/sileo/build-sileo.sh" "$JB/sileo-fork"
	P=$JB/root/var/jb/Applications/Sileo.app
	[[ -d $P ]] || die "no Sileo.app in the prefix at $P"
	rm -rf "$P"
	cp -a "$JB/sileo-fork/Sileo.app" "$P"
	echo fork > "$JB/.sileo-from"
	echo "prepare.sh: the prefix's Sileo.app is the fork's ($(head -1 "$JB/sileo-fork/SOURCE" | cut -d' ' -f2 | cut -c1-12))"
	;;
*) die "SILEO_FROM is procursus or fork, not '$SILEO_FROM'" ;;
esac
echo "prepare.sh: PACKAGE_MANAGER=${PACKAGE_MANAGER:-zebra} ($MANAGERS)"
