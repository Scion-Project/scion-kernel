#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 92-audio-bt.sh ROOT [undo] : what iOS needs on the root to use the S9's speakers, microphone and Bluetooth
# (tools/ios27/audio-bt/README.md), the kernel's side (abox=8 max98512=3 cs47l93=6 abox_mic=1 abox_rmem=1 bt=7) being on by
# default: tools/ios27/audio-bt/build.sh builds Scion's HAL plug-in, hakios-inject and the two Bluetooth libraries with the
# builder's Xcode (arm64e, then the PAC rewrite for the S9's cores, which needs PY with numpy), and
# tools/ios27/audio-bt/60-audio-bt.sh puts them on the root with BlueTool's scripts, the phone's Bluetooth patch and the jobs
# com.apple.audiomxd, com.apple.bluetoothd, com.apple.BlueTool back in launchd.plist (20-launchd.sh takes the first two out).
# No library of it is inserted through sweetinject= (bluetoothd and BlueTool start through hakios-inject).
#   HCD             the phone's own Bluetooth patch (bcm4361B2_semco.hcd or bcm4361B2_murata.hcd, from its VENDOR
#                   partition; never in a repository). Without it the step skips and the root is built without audio and
#                   Bluetooth (bluetoothd would restart for ever on a chip without its patch).
#   OS_MNT          the IPSW's OS image mounted: the root build gives it; by hand, needed only if the root lacks
#                   /System/Library/Audio or the jobs.
#   ENABLE_AUDIO_BT=0 off.   undo: everything taken off again, the jobs back where they were.
# Idempotent. The pieces were run on a phone together on 9 November 2026 (README.md, "Status"); this step and the kernel with
# all of it on by default have NOT been run on a phone.
set -euo pipefail
. "$(cd "$(dirname "$0")/.." && pwd)/feature.sh"
feature_setup 92-audio-bt AUDIO_BT
AB=$FEATURE_SK/tools/ios27/audio-bt

feature_inputs() {
	[[ -n ${HCD:-} ]] || feature_skip "set HCD to your phone's own Bluetooth patch (bcm4361B2_semco.hcd or bcm4361B2_murata.hcd from its VENDOR partition)"
	[[ -f $HCD ]] || feature_skip "HCD=$HCD is not a file"
	if [[ ! -d $R/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver && ! -d ${OS_MNT:-/nonexistent}/System/Library/Audio ]]; then
		feature_skip "the root has no /System/Library/Audio: set OS_MNT to the IPSW's OS image, mounted"
	fi
	feature_have_sdk || feature_skip "no Xcode with the iPhoneOS SDK: set DEVELOPER_DIR"
	feature_as_builder "${PY:-python3}" -c 'import numpy' 2>/dev/null || feature_skip "set PY to a Python with numpy (the PAC rewrite of build.sh needs it)"
}

feature_apply() {
	feature_build env OUT="$FEATURE_OUT" PY="${PY:-python3}" bash "$AB/build.sh" >/dev/null
	AUDIO_BT=1 FILES=$FEATURE_OUT bash "$AB/60-audio-bt.sh" "$R"
	feature_say "HakiosAudio.driver, hakios-inject, the Bluetooth libraries and BlueTool's scripts built and installed, $(basename "$HCD") in /private/var/hakios/bt, audiomxd, bluetoothd and BlueTool in launchd.plist"
}

feature_undo() {
	bash "$AB/60-audio-bt.sh" "$R" undo
	feature_say "undone"
}

feature_main "$@"
