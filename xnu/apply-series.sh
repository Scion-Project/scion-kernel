#!/bin/bash
# Get Apple's XNU sources (the release that Darwin userland 16.5 was built from) and put the HakiOS
# work on top of them: hakios.patch changes Apple's files, files/ holds the new ones.
# usage: xnu/apply-series.sh [DIR]              (default: $XNU_SRC, else the OS profile's tree: xnu-src/<xnu_tag> next to this directory)
# The series is the profile's base's: patches/<base>/series (patches/xnu-8796 for ios-16.5, patches/xnu-12377 for ios-26).
# env:   XNU_URL   where to fetch from (default: Apple's repository on GitHub; an existing clone
#                  that has the tag will do: XNU_URL=file:///path/to/xnu)
# The result is a git tree on the branch "hakios" with the work uncommitted, which is how
# xnu/export.sh expects to find it. A directory that already exists is left alone.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python3 "$HERE/../tools/os-profile.py" require || exit 1
DIR=${1:-${XNU_SRC:-$HERE/../$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get src)}}
URL=${XNU_URL:-https://github.com/apple-oss-distributions/xnu.git}
TAG=$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get xnu_tag)
BASE=$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get xnu_commit)

[ -e "$DIR" ] && { echo "apply.sh: $DIR already exists; not touching it" >&2; exit 1; }
BASEID=$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get base)
HFS=$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get hfs)
SERIES=$HERE/../patches/$BASEID
[ -s "$SERIES/series" ] || { echo "apply-series.sh: no $SERIES/series" >&2; exit 1; }
[ -f "$HERE/../drivers/map" ] || { echo "apply.sh: no drivers/map next to xnu/ (the drivers are in drivers/)" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
git init -q "$DIR"
trap 'echo "apply-series.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
cd "$DIR"
git remote add origin "$URL"
git fetch -q --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"      # that one release, without the history
git checkout -q -b hakios "$TAG"
[ "$(git rev-parse HEAD)" = "$BASE" ] || { echo "apply.sh: $TAG is $(git rev-parse HEAD), expected $BASE" >&2; false; }
echo 'BUILD/' >> .git/info/exclude
# files Apple's tag includes but does not ship, taken from another open-source release of XNU: patches/<base>/imports,
# one line each: "<tag> <commit> <path>" (fetched like the tag itself, and checked against the commit)
if [ -s "$SERIES/imports" ]; then
	grep -v '^#' "$SERIES/imports" | while read -r itag icommit ipath; do
		[ -n "$itag" ] || continue
		git fetch -q --depth 1 origin "refs/tags/$itag:refs/tags/$itag"
		[ "$(git rev-parse "$itag^{commit}")" = "$icommit" ] || { echo "apply-series.sh: $itag is not $icommit" >&2; exit 1; }
		git checkout -q "$itag" -- "$ipath"
	done
fi
grep -v '^#' "$SERIES/series" | while read -r p; do
	git apply --whitespace=nowarn "$SERIES/$p" || { echo "apply-series.sh: $p does not apply" >&2; exit 1; }
done
# HFS Plus (the data volume): Apple's release, fetched like XNU, and this port's changes over it
if [ "$HFS" = yes ]; then "$HERE/hfs/hfs.sh" put "$DIR"; fi
# the drivers are kept by subsystem (drivers/<subsystem>/), not at their place in Apple's tree: drivers/map says where
# each one goes, one line per file: "<subsystem> <path in the tree>"
NDRIVERS=0
while read -r sub tree _; do
	case "$sub" in ''|'#'*) continue ;; esac
	[ -f "$HERE/../drivers/$sub/$(basename "$tree")" ] || { echo "apply.sh: drivers/$sub/$(basename "$tree") is in drivers/map but missing" >&2; false; }
	mkdir -p "$(dirname "$tree")"
	cp -p "$HERE/../drivers/$sub/$(basename "$tree")" "$tree"
	NDRIVERS=$((NDRIVERS + 1))
done < "$HERE/../drivers/map"
# the drivers of the SoC and device repositories checked out (socs/*/drivers/, devices/*/drivers/)
python3 "$HERE/../tools/hakios-config.py" external-files | while IFS="$(printf '\t')" read -r src tree; do
	mkdir -p "$(dirname "$tree")"
	cp -p "$src" "$tree"
done
# what the numbered patches change in the drivers today, until it is folded into them
if [ -s "$SERIES/9000-driver-fixups.patch" ]; then git apply --whitespace=nowarn "$SERIES/9000-driver-fixups.patch"; fi
# which of them are built is the configuration's (drivers/*/conf, CONFIG_HAKIOS_*): put into the tree as XNU reads it.
# What that writes is made again for every build, so it is kept out of the work that xnu/export.sh saves.
printf '%s\n' config/MASTER.hakios bsd/dev/hakios.drivers.h >> .git/info/exclude
# Into XNU's configuration where the base's series has made a place for them (the HakiOS block of bsd/conf/files, read with
# config/MASTER.hakios): both series do. Which drivers a base gets is drivers/*/conf's `bases` (docs/two-bases.md).
if grep -qF "# HakiOS drivers: made by tools/hakios-config.py kernel" bsd/conf/files; then
	python3 "$HERE/../tools/hakios-config.py" kernel --tree "$DIR"
else
	echo "apply-series.sh: $BASEID: the drivers are in the tree but not in its configuration (docs/two-bases.md)"
fi
echo "apply-series.sh: $DIR: $TAG + the series + $NDRIVERS drivers"
