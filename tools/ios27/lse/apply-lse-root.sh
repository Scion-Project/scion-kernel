#!/usr/bin/env bash
# apply-lse-root.sh MOUNTED_ROOT [LOG]
# Rewrites the LSE atomics of the iOS 27 shared cache on an attached root (already run through a64e-rewrite.py: the arm64e
# translation) into LL/SC thunks, in place, then verifies the result. Several paths of the root name the same cache file (the
# cryptex copy is hard-linked), so one run covers both. Needs write access to the cache files (attach the root with -owners off, or
# run with sudo). About a minute, plus the check. Running it again is harmless: rewritten sites are B instructions and are skipped.
# Undo: tools/ios27/lse/restore-lse-root.sh MOUNTED_ROOT LOG
# env:  PY     the Python 3 with numpy that runs the tools (default python3)
#       SKIP   substrings of image paths to leave alone (default libcorecrypto, whose FIPS self-check hashes its own code)
# The log (default build/lse/lse-root-<UTC time>.log) is what restore and verify need: keep it with the root.
set -euo pipefail
R=${1:?mounted root}
C="$R/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e"
[[ -f $C ]] || { echo "no cache at $C"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-python3}
LOG=${2:-$HERE/../../../build/lse/lse-root-$(date -u +%Y%m%dT%H%M%SZ).log}
mkdir -p "$(dirname "$LOG")"
# shellcheck disable=SC2086
"$PY" -I "$HERE/lse-rewrite.py" cache "$C" --skip ${SKIP:-libcorecrypto} --log "$LOG"
"$PY" -I "$HERE/lse-rewrite.py" verify "$C" --log "$LOG"
echo "LOG=$LOG"
sync
