#!/usr/bin/env bash
# HID event monitor for the phone's iOS root -> build/touchspy/touchspy
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/touchspy
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
xcrun -sdk iphoneos clang -arch arm64e -mios-version-min=18.0 -O1 -Wall \
	-framework CoreFoundation -framework IOKit -o build/touchspy/touchspy tools/ios27/touchspy/touchspy.c
# The S9 has no PAC: rewrite the arm64e instructions as a64e-rewrite does the cache.
build/venv/bin/python -I tools/ios27/a64e/a64e-rewrite.py macho build/touchspy/touchspy | tail -1
codesign -s - -f -i com.starbite.touchspy --entitlements tools/ios27/touchspy/touchspy.entitlements build/touchspy/touchspy
otool -L build/touchspy/touchspy | tail -n +2
