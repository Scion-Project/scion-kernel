#!/bin/bash
# harvest-m2v-cache.sh PULLED OUT : a warm cache for steps.d/81-m2v-cache.sh from a phone's /private/var/local/m2v-cache.
#   PULLED = that directory, copied off the phone after a warm-up with the bundle that will ship (open Settings, Display & Brightness
#   / Appearance, the app switcher, Control Center, a few apps; wait until `PANVK: ... compiled` lines stop). Keeps, per user
#   directory (u<uid>/), the BUILD directories under v1/ that were written last (the running bundle's: its translated shaders)
#   and the content directory (PanVK's pipeline data, valid for as long as PanVK is the same); drops older builds' directories.
#   OUT (default tools/ios27/fullroot/warm-m2v) is what the step installs; it is not in git (cache entries are build artefacts).
set -euo pipefail
SRC=${1:?pulled m2v-cache directory}
OUT=${2:-$(cd "$(dirname "$0")/../fullroot" && pwd)/warm-m2v}
# the content directory's name is fixed: sha256 of the tag the runtime names it by
CONTENT=$(printf 'metal2vulkan shader cache content\0' | shasum -a 256 | cut -d' ' -f1)
rm -rf "$OUT"; mkdir -p "$OUT"
n=0
for u in "$SRC"/u[0-9]*/; do
    [[ -d $u/v1 ]] || continue
    name=$(basename "$u")
    mkdir -p "$OUT/$name/v1"
    # newest build directory by the newest file in it
    newest=""; best=0
    for b in "$u"v1/*/; do
        id=$(basename "$b"); [[ $id == "$CONTENT" ]] && continue
        t=$(find "$b" -type f -exec stat -f %m {} + 2>/dev/null | sort -n | tail -1); t=${t:-0}
        if (( t >= best )); then best=$t; newest=$id; fi
    done
    [[ -n $newest ]] && cp -R "$u/v1/$newest" "$OUT/$name/v1/$newest"
    [[ -d $u/v1/$CONTENT ]] && cp -R "$u/v1/$CONTENT" "$OUT/$name/v1/$CONTENT"
    n=$((n + 1))
    echo "harvest: $name: build ${newest:-none}, content $([[ -d $u/v1/$CONTENT ]] && echo yes || echo NO), $(find "$OUT/$name" -type f | wc -l) files"
done
(( n )) || { echo "harvest: no u<uid>/ directory with v1/ in $SRC (a phone that ran the older per-program plug-in has backboardd/ instead: boot the new bundle first)"; exit 1; }
du -sk "$OUT" | awk '{print "harvest: " $1 " KiB in '"$OUT"'"}'
