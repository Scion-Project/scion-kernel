#!/usr/bin/env bash
# iOS 26 kernel as an OS store entry (no BOOT image): the 12377 kernel (strip -x
# copy, as tools/mkimage-ios26.sh) + the iPhone 11 ADT + CMDLINE_FILE, packed by
# mkpayload.py into a SWEETXNU payload for tools/store/sbstore2.py (type xnu).
#   [ADT=...] [KERNEL=...] tools/mkpayload-ios26.sh NAME CMDLINE_FILE   -> out/xnu-NAME.xnu
# The payload holds at most 607 cmdline bytes and mkpayload.py cuts longer ones
# silently (the last arguments, e.g. sweetreboot=1, would vanish): a longer
# cmdline is refused here, and the packed cmdline is read back and compared.
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
name=$1 cmdline=$2
KERNEL=${KERNEL:-out/kernel.development.starlte-12377}
ADT=${ADT:-out/starlte-iphone11.adt}
MAX_CMDLINE=607                         # mkpayload.py BOOT_LINE_LENGTH - 1
[ -f "$KERNEL" ] || { echo "no $KERNEL: tools/build-xnu-12377.sh"; exit 1; }
line=$(cat "$cmdline")
case "$line" in *$'\n'*) echo "$cmdline: more than one line"; exit 1 ;; esac
n=$(printf '%s' "$line" | wc -c | tr -d ' ')
[ "$n" -le "$MAX_CMDLINE" ] || { echo "$cmdline: cmdline $n bytes > $MAX_CMDLINE (it would be cut)"; exit 1; }
trap 'rm -f "$KERNEL.x"' EXIT           # the 21 MB stripped copy is only an input
cp "$KERNEL" "$KERNEL.x"
xcrun strip -x "$KERNEL.x"
python3 ref/ios-herolte/tools/mkpayload.py "$KERNEL.x" "$ADT" "out/xnu-$name.xnu" \
    --cmdline "$line" | grep -E "^(size|fixups|entry)" || true
python3 - "out/xnu-$name.xnu" "$line" <<'PY'
import sys
blob = open(sys.argv[1], 'rb').read(0x48 + 608)
assert blob[:8] == b'SWEETXNU', 'not a SWEETXNU payload'
packed = blob[0x48:0x48 + 608].split(b'\0')[0].decode()
if packed != sys.argv[2]:
    sys.exit(f'{sys.argv[1]}: packed cmdline ({len(packed)} bytes) differs from the file ({len(sys.argv[2])})')
print(f'cmdline: {len(packed)} of 607 bytes, read back from the payload')
PY
shasum -a 256 "out/xnu-$name.xnu"
