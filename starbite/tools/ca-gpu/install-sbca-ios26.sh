#!/usr/bin/env bash
# Put the GPU libraries on the phone's iOS 26 root (TWRP): out/ios/libsbca.dylib as
# /usr/lib/sbca.dylib, libsbgl.dylib and libOSMesa.8.dylib in /usr/local/starbite/lib;
# clears sbca's log and shots. Edits build/root-ios26.img and pushes only the changed
# chunks; --pull first pulls the live root (needed when the phone booted since the last
# pull/push). backboardd loads sbca with boot-arg sweetinject=/usr/lib/sbca.dylib,backboardd
# (and, being arm64e, takes these arm64 libraries because of xnu patch 0324).
#   tools/ca-gpu/install-sbca-ios26.sh [--pull]
set -euo pipefail
cd "$(dirname "$0")/../.."
IMG=build/root-ios26.img
if [ "${1:-}" = --pull ]; then
	python3 tools/starlte-root-sync.py pull $IMG build/root-ios26-b.img | tail -1
	mv build/root-ios26-b.img $IMG
	mv build/root-ios26-b.img.hashes $IMG.hashes
fi
R=$(hdiutil attach -owners off -nobrowse $IMG 2>/dev/null | awk -F'\t' '/Volumes/{print $NF}')
[ -n "$R" ] || { echo "attach failed"; exit 1; }
mkdir -p "$R/usr/local/starbite/lib"
cp out/ios/libsbca.dylib "$R/usr/lib/sbca.dylib"
cp out/ios/libsbgl.dylib out/ios/libOSMesa.8.dylib "$R/usr/local/starbite/lib/"
chmod 755 "$R/usr/lib/sbca.dylib" "$R"/usr/local/starbite/lib/*.dylib
rm -f "$R"/private/var/tmp/sbca.log "$R"/private/var/tmp/sbca-*.log "$R"/private/var/tmp/sbca-shot-*
sync
hdiutil detach "$R" >/dev/null
python3 tools/starlte-root-sync.py push $IMG | tail -1
