#!/bin/bash
# The kernel tree of a profile on an XNU base other than xnu-8796 (ios-26, ios-27): Scion-Project/xnu at the commit the profile pins,
# with Scion's changes as commits on Apple's tag (docs/xnu-fork.md), and then what is not in the fork: the drivers and the
# configuration, as xnu/apply.sh adds them.
# usage: xnu/apply-series.sh [DIR]              (default: $XNU_SRC, else the OS profile's tree: xnu-src/<xnu_tag> next to this directory)
# env:   XNU_FORK_URL  where to fetch the fork from (default the profile's xnu_fork; a clone that has the commit will do)
# A profile whose `drivers` is none (ios-27) takes no driver from drivers/: its fork's branch carries them, apart from the files its
# `fork_copy` names. The result is a git tree on the branch "hakios", the fork's commit checked out. A directory that already
# exists is left alone.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python3 "$HERE/../tools/os-profile.py" require || exit 1
DIR=${1:-${XNU_SRC:-$HERE/../$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get src)}}

. "$HERE/fork.sh"
[ -e "$DIR" ] && { echo "apply.sh: $DIR already exists; not touching it" >&2; exit 1; }
BASEID=$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get base)
[ -f "$HERE/../drivers/map" ] || { echo "apply.sh: no drivers/map next to xnu/ (the drivers are in drivers/)" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
trap 'echo "apply-series.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
# Scion-Project/xnu at the profile's commit: Apple's tag with the imports, Scion's changes and what its scripts stage, as commits
fork_checkout "$DIR"
NDRIVERS=0
if [ "$(_scion_profile_get drivers || true)" = none ]; then
echo "apply-series.sh: this profile carries its own drivers (drivers none), none is taken from drivers/"
else
# the drivers are kept by subsystem (drivers/<subsystem>/), not at their place in Apple's tree: drivers/map says where
# each one goes, one line per file: "<subsystem> <path in the tree>"
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
# which of them are built is the configuration's (drivers/*/conf, CONFIG_HAKIOS_*): put into the tree as XNU reads it.
# What that writes is made again for every build, so it is kept out of git status.
printf '%s\n' config/MASTER.hakios bsd/dev/hakios.drivers.h >> .git/info/exclude
# Into XNU's configuration where the base's fork has made a place for them (the HakiOS block of bsd/conf/files, read with
# config/MASTER.hakios): both do. Which drivers a base gets is drivers/*/conf's `bases` (docs/two-bases.md).
if grep -qF "# HakiOS drivers: made by tools/hakios-config.py kernel" bsd/conf/files; then
	python3 "$HERE/../tools/hakios-config.py" kernel --tree "$DIR"
else
	echo "apply-series.sh: $BASEID: the drivers are in the tree but not in its configuration (docs/two-bases.md)"
fi
fi
echo "apply-series.sh: $DIR: Scion-Project/xnu ${FORK_COMMIT:0:12} + $NDRIVERS drivers"
