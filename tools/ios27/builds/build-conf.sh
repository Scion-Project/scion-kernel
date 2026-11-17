# SPDX-License-Identifier: MIT-0
# build-conf.sh : sourced by the iOS 27 tools (never run on its own). Which iOS build the tools are for is said in ONE place:
#   tools/ios27/builds/DEFAULT        one line: the build every tool uses when nothing says otherwise (today 24A446)
#   tools/ios27/builds/<BUILD>.conf   what the tools need to know of that build: KEY=VALUE lines, read here and never executed
# IOS27_BUILD=<BUILD> in the environment picks another build that has a .conf (through sudo: name it on sudo's line).
# A build whose STATUS is not "supported" is one being brought up (docs/ios27/UPDATING-IOS.md): the tools say so in one line,
# and a step that needs something of it that is not written down yet (its patch list) refuses.
# The checks that refuse a MIXTURE of builds (inputs of one build, a private directory or a rewritten cache of another) are
# ios27_build_same below, called by tools/ios27/s9/build-root.sh and tools/ios27/fullroot/build-full-root.sh.
# Plain sh/bash 3.2 (macOS's /bin/bash): no arrays, no ${x,,}.
IOS27_BUILDS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# the default build (tools/ios27/builds/DEFAULT)
ios27_build_default() {
	local b=
	read -r b < "$IOS27_BUILDS_DIR/DEFAULT" || [ -n "$b" ] || return 1
	echo "$b"
}

# the builds that have a .conf, in name order, separated by spaces
ios27_build_list() {
	local f out=
	for f in "$IOS27_BUILDS_DIR"/*.conf; do [ -f "$f" ] || continue; f=${f##*/}; out="$out${out:+ }${f%.conf}"; done
	echo "$out"
}

# ios27_build_get BUILD KEY : KEY's value in BUILD's .conf ("" when the key is absent; a trailing "# note" is not part of it);
# fails when BUILD has no .conf
ios27_build_get() {
	local f="$IOS27_BUILDS_DIR/$1.conf" k v
	[ -f "$f" ] || return 1
	while IFS='=' read -r k v || [ -n "$k" ]; do
		case "$k" in '' | \#*) continue ;; esac
		[ "$k" = "$2" ] || continue
		v=${v%%#*}
		v="${v%"${v##*[![:space:]]}"}"
		echo "$v"
		return 0
	done < "$f"
	echo ""
}

# ios27_build_check BUILD : BUILD has a .conf (else a message and failure); a build that is not "supported" is named on stderr
ios27_build_check() {
	local s
	if [ -z "$1" ] || [ ! -f "$IOS27_BUILDS_DIR/$1.conf" ]; then
		echo "no tools/ios27/builds/${1:-?}.conf: the iOS 27 tools know the builds: $(ios27_build_list) (docs/ios27/UPDATING-IOS.md)" >&2
		return 1
	fi
	s=$(ios27_build_get "$1" STATUS)
	[ "$s" = supported ] || echo "NOTE: build $1 is ${s:-of no status} in tools/ios27/builds/$1.conf: not booted on a phone with these tools" >&2
	return 0
}

# ios27_build_patches BUILD : the absolute path of BUILD's cache patch list; fails (and says so) when it has none yet
ios27_build_patches() {
	local p
	p=$(ios27_build_get "$1" PATCHES) || return 1
	if [ -z "$p" ] || [ ! -f "$IOS27_BUILDS_DIR/../$p" ]; then
		echo "build $1 has no cache patch list yet (PATCHES in tools/ios27/builds/$1.conf): each word has to be found again in its cache (docs/ios27/UPDATING-IOS.md)" >&2
		return 1
	fi
	echo "$(cd "$IOS27_BUILDS_DIR/.." && pwd)/$p"
}

# ios27_build_of_inputs DIR : the build fetch-apple-inputs.sh recorded in DIR/SOURCE.txt ("... build 24A446"); "" without one
ios27_build_of_inputs() {
	local l=
	[ -f "$1/SOURCE.txt" ] || { echo ""; return 0; }
	read -r l < "$1/SOURCE.txt" || true
	case "$l" in *", build "*) l=${l##*, build }; echo "${l%% *}" ;; *) echo "" ;; esac
}

# ios27_build_of_root ROOT : ProductBuildVersion of ROOT/System/Library/CoreServices/SystemVersion.plist; "" without one
ios27_build_of_root() {
	local p="$1/System/Library/CoreServices/SystemVersion.plist"
	[ -f "$p" ] || { echo ""; return 0; }
	"${PY:-python3}" -I -c 'import plistlib, sys
try: print(plistlib.load(open(sys.argv[1], "rb")).get("ProductBuildVersion") or "")
except Exception: print("")' "$p"
}

# ios27_build_same WHAT_A BUILD_A WHAT_B BUILD_B : fails, saying which two disagree, when both builds are known and differ
ios27_build_same() {
	[ -z "$2" ] || [ -z "$4" ] || [ "$2" = "$4" ] && return 0
	echo "a mixture of iOS builds: $1 is $2, $3 is $4. Every input of one root must come from one build (docs/ios27/UPDATING-IOS.md)" >&2
	return 1
}
