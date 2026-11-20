#!/bin/bash
# 81-m2v-cache.sh ROOT : the Metal shader cache directory, and a warm cache in it.
#   The plug-in (StarbiteMetal.bundle, HotMetal2.mm + patches/hotmetal2-shared-cache.diff) keeps one cache per USER, shared by every
#   process of that user: /private/var/local/m2v-cache/u<uid>/ (v1/<build>/ = translated shaders, v1/<content>/ = PanVK's pipeline data;
#   all named by content). It makes the directory itself, but only where /private/var/local/m2v-cache exists and is writable: this
#   step makes it (0777), and removes the per-program directories (backboardd/, PosterBoard/, ...) older plug-ins left.
#   WARM_M2V_CACHE=<dir> (default tools/ios27/fullroot/warm-m2v when it exists) holds u<uid>/ directories as harvest-m2v-cache.sh
#   makes them from a phone's cache after a warm-up (Settings, Display, Liquid Glass, the app switcher...): they are copied in, owned
#   by that uid, owner-only (the runtime refuses files others can write), so the first Settings open compiles nothing.
#   A warm cache is valid for the bundle and PanVK it was harvested with (the entries live under the bridge's build hash and
#   PanVK's cache UUID); another build finds none of it and compiles as before, leaving the stale files to the runtime's 30-day sweep.
#   WARM_M2V_CACHE=none: no warm cache (and the existing directories are left as they are).
set -euo pipefail
R=$1
H=$(cd "$(dirname "$0")" && pwd)
L=$R/private/var/local
C=$L/m2v-cache
install -d -m 0777 "$L" 2>/dev/null || mkdir -p "$L"
install -d -m 0777 "$C" 2>/dev/null || mkdir -p "$C"
chmod 0777 "$C"
# a plug-in before the shared cache: one directory per program
for d in "$C"/*/; do
    [[ -d $d ]] || continue
    n=$(basename "$d")
    case $n in u[0-9]*) ;; *) rm -rf "$d"; echo "81-m2v-cache: removed the per-program cache $n";; esac
done
WARM=${WARM_M2V_CACHE:-$H/../warm-m2v}
if [[ $WARM == none || ! -d $WARM ]]; then
    echo "81-m2v-cache: $C ready (0777); no warm cache (WARM_M2V_CACHE unset or $H/../warm-m2v absent)"
    exit 0
fi
files=0
for u in "$WARM"/u[0-9]*/; do
    [[ -d $u ]] || continue
    uid=$(basename "$u"); uid=${uid#u}
    rm -rf "$C/u$uid"
    mkdir -p "$C/u$uid"
    cp -R "$u." "$C/u$uid/"
    find "$C/u$uid" -type d -exec chmod 0700 {} +
    find "$C/u$uid" -type f -exec chmod 0600 {} +
    chown -R "$uid:$uid" "$C/u$uid" 2>/dev/null || echo "81-m2v-cache: u$uid: chown to $uid failed (not root?): the files stay with the building user"
    files=$((files + $(find "$C/u$uid" -type f | wc -l)))
done
echo "81-m2v-cache: warm cache from $WARM: $files files, $(du -sk "$C" | cut -f1) KiB in $C"
