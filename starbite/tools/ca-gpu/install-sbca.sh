#!/usr/bin/env bash
# Put out/ios/libsbca.dylib and libsbgl.dylib on the phone's iOS root (TWRP), clear sbca's log
# and shots. Edits build/root-phone.img and pushes only the changed chunks; --pull first pulls
# the live root (needed when the phone booted since the last pull/push, e.g. after a test boot
# that was not followed by tools/ca-gpu/fetch-shots.sh).
#   tools/ca-gpu/install-sbca.sh [--pull]
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ "${1:-}" = --pull ]; then
	python3 tools/starlte-root-sync.py pull build/root-phone.img build/root-phone-b.img | tail -1
	mv build/root-phone-b.img build/root-phone.img
	mv build/root-phone-b.img.hashes build/root-phone.img.hashes
fi
hdiutil attach -owners off build/root-phone.img 2>/dev/null | tail -1
R=/Volumes/IOSROOT
mkdir -p $R/usr/local/starbite/lib
cp out/ios/libsbca.dylib $R/usr/lib/sbca.dylib
cp out/ios/libsbgl.dylib $R/usr/local/starbite/lib/libsbgl.dylib
chmod 755 $R/usr/lib/sbca.dylib $R/usr/local/starbite/lib/libsbgl.dylib
rm -f $R/private/var/tmp/sbca.log $R/private/var/tmp/sbca-*.log $R/private/var/tmp/sbca-shot-*
sync
hdiutil detach $R 2>/dev/null | tail -1
python3 tools/starlte-root-sync.py push build/root-phone.img | tail -1
