#!/usr/bin/env bash
# restore-lse-root.sh MOUNTED_ROOT LOG: undo apply-lse-root.sh (every original instruction word goes back; the thunks stay as unused bytes)
set -euo pipefail
C="${1:?mounted root}/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e"
HERE=$(cd "$(dirname "$0")" && pwd)
"${PY:-python3}" -I "$HERE/lse-rewrite.py" restore "$C" --log "${2:?log of the apply run}"
sync
