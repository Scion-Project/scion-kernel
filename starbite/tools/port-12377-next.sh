#!/usr/bin/env bash
# Replay the 11417 port tree's commits (build/xnu-11417, HEAD) onto the
# xnu-12377 port tree (build/xnu-12377, branch port-12377), one cherry-pick
# each, in order, skipping ones already picked (matched by subject). Stops at
# the first conflict: resolve, `git add`, `git -c core.editor=true cherry-pick
# --continue` (or --skip for an obsolete patch, then note it), and run again.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
OLD=$P/build/xnu-11417 NEW=$P/build/xnu-12377
g() { git -C "$NEW" -c user.name=starbite -c user.email=starbite@local "$@"; }
done_list=$(g log --format=%s 5a05264c..HEAD)
git -C "$OLD" log --reverse --format='%H %s' 3b61c9e..HEAD | while read -r sha subj; do
    case "$subj" in stage:*) continue ;; esac
    grep -qxF "$subj" <<<"$done_list" && continue
    [ -f "$P/build/port-12377-skipped.txt" ] && grep -qxF "$subj" "$P/build/port-12377-skipped.txt" && continue
    if g cherry-pick --allow-empty "$sha" >/dev/null 2>&1; then
        echo "ok   $subj"
    else
        echo "STOP $subj ($sha)"
        g status --short | grep -E '^(UU|AA|DU|UD|AU|UA|DD) ' || true
        exit 2
    fi
done
