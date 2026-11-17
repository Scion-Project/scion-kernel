#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 93-modem.sh ROOT [undo] : what iOS needs on the root to see the S9's modem (docs/ios27/MODEM-IOS.md), the kernel's side
# (cp=7 cp_nv=2 cp_delay=40 cp_rfs=1 cp_if=1) being on by default: tools/ios27/modem/build.sh builds scion-celld and the
# status bar's library c.dylib with the builder's Xcode, and tools/ios27/modem/66-celld.sh installs them (scion-celld and its
# job com.scion.celld; /usr/lib/c.dylib, on while /private/var/local/scion-bars exists), c.dylib recorded for insertion into
# SpringBoard (the record next to the image; make-boot-image.sh then adds ";/usr/lib/c.dylib,SpringBoard" to sweetinject=).
# Not the test job (66-celld.sh ... test) nor scion-fetch: those are for a test root only, by hand.
# Data needs the operator's cp_apn= on the boot image's command line (--cmdline); without it the bars and the network's name
# show and there is no data. No calls and no SMS.
#   ENABLE_MODEM=0  off.   undo: 66-celld.sh undo and the record.
# Idempotent. The two were run on a phone (14 November 2026, on the modem's own branch); this step and the kernel with the
# modem on by default have NOT been run on a phone.
set -euo pipefail
. "$(cd "$(dirname "$0")/.." && pwd)/feature.sh"
feature_setup 93-modem MODEM
M=$FEATURE_SK/tools/ios27/modem
LIB=/usr/lib/c.dylib

feature_inputs() {
	feature_have_sdk || feature_skip "no Xcode with the iPhoneOS SDK: set DEVELOPER_DIR"
}

feature_apply() {
	feature_build sh "$M/build.sh" "$FEATURE_OUT" >/dev/null
	[[ -f $FEATURE_OUT/scion-celld && -f $FEATURE_OUT/c.dylib ]] || feature_die "build.sh made no scion-celld or c.dylib"
	bash "$M/66-celld.sh" "$R" "$FEATURE_OUT" add
	[[ $EUID != 0 ]] || chown 0:0 "$R$LIB"
	feature_inject "$LIB" SpringBoard
	feature_say "scion-celld and its job com.scion.celld, $LIB (sha256 $(shasum -a 256 "$R$LIB" | cut -c1-16)) to be inserted into SpringBoard"
}

feature_undo() {
	feature_uninject "$LIB"
	bash "$M/66-celld.sh" "$R" "" undo
	feature_say "undone"
}

feature_main "$@"
