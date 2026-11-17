#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 91-wifi.sh ROOT [undo] : what iOS needs on the root to use the S9's Wi-Fi (docs/ios27/WIFI.md), the kernel's side
# (wifi_pcie=6 brcmfmac=8, with brcmfmac linked in: tools/mk-brcmfmac.sh before the kernel) being on by default:
#   tools/ios27/wifi/60-wifi-firmware.sh   the phone's own firmware, NVRAM and CLM into /private/var/hakios/wifi/
#   tools/ios27/wifi/62-wifid-loadable.sh  launchd may start wifid on the S9 (WIFI_WIFID=0 leaves this one out)
#   tools/ios27/wifi/63-wifi-netservice.sh configd's Wi-Fi service (DHCP at once, not after minutes), on a root that has
#                                          configd's preferences; a root that has never booted has none, and is left so
# Not the test job tools/ios27/wifi/64-wifi-nettest.sh: that is for a test root only, by hand.
#   WIFI_FW_DIR     the directory with the phone's own files (scion-starlte tools/fetch-firmware.sh fills one); never in a
#                   repository. Without it the step skips and the root is built without Wi-Fi.
#   WIFI_EXTRAS=1   also mac.info and network (a network's name and passphrase) from WIFI_FW_DIR: the image then carries
#                   the passphrase (a test root, kept private)
#   WIFI_FW_SUMS    default devices/starlte/firmware/wifi/SHA256SUMS (python3 tools/hakios-device.py sync)
#   ENABLE_WIFI=0   off.   undo: the three taken off the root again.
# Idempotent. Each of the three was run on a phone on its own (10 and 13 November 2026; wifid through the ADT's wlan node,
# not through 62's key removal); this step, and the three together, have NOT been run on a phone.
set -euo pipefail
. "$(cd "$(dirname "$0")/.." && pwd)/feature.sh"
feature_setup 91-wifi WIFI
W=$FEATURE_SK/tools/ios27/wifi
PREFS=private/var/preferences/SystemConfiguration

feature_inputs() {
	[[ -n ${WIFI_FW_DIR:-} ]] || feature_skip "set WIFI_FW_DIR to the directory that holds your phone's own Wi-Fi files"
	[[ -d $WIFI_FW_DIR ]] || feature_skip "WIFI_FW_DIR=$WIFI_FW_DIR is not a directory"
	[[ -n ${WIFI_FW_SUMS:-} || -f $FEATURE_SK/devices/starlte/firmware/wifi/SHA256SUMS ]] ||
		feature_skip "set WIFI_FW_SUMS to scion-starlte's firmware/wifi/SHA256SUMS (or run python3 tools/hakios-device.py sync)"
}

feature_apply() {
	bash "$W/60-wifi-firmware.sh" "$R"
	WIFI_WIFID=${WIFI_WIFID:-1} bash "$W/62-wifid-loadable.sh" "$R"
	local svc="configd's Wi-Fi service"
	if [[ -d $R/$PREFS ]]; then
		bash "$W/63-wifi-netservice.sh" "$R"
	else
		# a root that has never booted has no configd preferences: iOS makes the service at the first Wi-Fi boot, and DHCP
		# waits some minutes that once (WIFI.md); writing configd's first preferences ourselves has not been tried
		svc="no configd preferences yet (a root that has not booted): iOS makes the Wi-Fi service at the first boot"
	fi
	feature_say "the phone's firmware in /private/var/hakios/wifi, $([[ ${WIFI_WIFID:-1} == 1 ]] && echo 'wifid loadable' || echo 'wifid left out (WIFI_WIFID=0)'), $svc$([[ ${WIFI_EXTRAS:-0} == 1 && -f $WIFI_FW_DIR/network ]] && echo '; a network and its passphrase (WIFI_EXTRAS=1): keep this image private')"
}

feature_undo() {
	[[ ! -f $R/$PREFS/preferences.plist ]] || bash "$W/63-wifi-netservice.sh" "$R" undo
	bash "$W/62-wifid-loadable.sh" "$R" undo
	bash "$W/60-wifi-firmware.sh" "$R" undo
	feature_say "undone"
}

feature_main "$@"
