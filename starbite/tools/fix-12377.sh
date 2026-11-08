#!/usr/bin/env bash
# Amend the port tree's uncommitted change into its newest patch (HEAD, e.g.
# patch: 0319-sweet-arm64e-userland.patch), add NOTE to its message, apply the
# same diff to the darwin-xnu-build tree (incremental rebuild) and re-export
# that patch into xnu/series-12377.
#   tools/fix-12377.sh "file.c (0xxx): what changed upstream"
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
PORT=$P/build/xnu-12377 DXB=$P/build/dxb-12377 OUT=$P/xnu/series-12377
g() { git -C "$PORT" -c user.name=starbite -c user.email=starbite@local "$@"; }
subj=$(g log -1 --format=%B | head -1)
case "$subj" in "patch: "*) name=${subj#patch: } ;; *) echo "HEAD is not a patch commit: $subj"; exit 1 ;; esac
diff=$(mktemp)
g add -A; g diff --cached > "$diff"
[ -s "$diff" ] || { echo "nothing to fix"; exit 1; }
git -C "$DXB/xnu" apply --check "$diff"
msg=$(g log -1 --format=%B)
printf '%s\n- %s\n' "$msg" "$1" | g commit -q --amend -F -
git -C "$DXB/xnu" apply "$diff"; rm "$diff"
g format-patch -q --stdout -1 --no-signature HEAD > "$OUT/$name"
cat "$OUT"/*.patch "$OUT"/*.sh | shasum -a 256 | cut -c1-16 > "$DXB/.series-12377"
echo "amended $name; build tree updated; series re-exported"
