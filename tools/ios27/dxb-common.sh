# tools/ios27/dxb-common.sh: sourced, not run. What the builders of the ios-27 profile share (ROOT is the repository's root, set by
# the script that sources this): where blacktop's darwin-xnu-build ("dxb", MIT) lives, what the build needs from the machine, and how
# its directory is made. Sourced by tools/ios27/build-xnu-ios.sh (what `OS_PROFILE=ios-27 make kernel` runs: docs/ios27/IOS27-TARGET.md)
# and by tools/ios27/build-xnu-12377.sh (the macOS-target build of the same tree).
#
# build/dxb-12377 is made once: dxb at the commit tools/ios27/dxb.pin names, its build.sh edited the way this profile's build needs
# (plain arm64 firehose without ARMv8.1 atomics, -j4, patches skipped), and the profile's kernel tree put in it as dxb/xnu by
# xnu/apply-series.sh (the fork's commit, what the profile's fork_stage scripts stage, the files of fork_copy). It is reused
# afterwards (make is incremental); what it was made from is in .series-12377, and a profile that has changed since asks for FRESH=1.

DXB=$ROOT/build/dxb-12377
# shellcheck source=tools/ios27/dxb.pin
. "$ROOT/tools/ios27/dxb.pin"

dxb_die() { echo "${DXB_WHO:-dxb}: $*" >&2; exit 1; }

# dxb_env: the machine and the Apple inputs the build needs, or a message that says which is missing
dxb_env() {
	[ "$(uname -s)" = Darwin ] || dxb_die "this builds on macOS (Xcode's clang and SDKs, Apple's Kernel Debug Kit); this is $(uname -s)"
	[ -z "${TOOLS_BIN:-}" ] || export PATH=$TOOLS_BIN:$PATH   # a directory with jq, gum, xcodes, cmake, ninja if they are not on PATH
	[ -n "${DEVELOPER_DIR:-}" ] || dxb_die "DEVELOPER_DIR is not set: set it to an Xcode (16.4 builds xnu-12377; Xcode 26 if its clang or SDK is too old), e.g. /Applications/Xcode.app/Contents/Developer"
	[ -d "$DEVELOPER_DIR" ] || dxb_die "Xcode missing at DEVELOPER_DIR=$DEVELOPER_DIR"
	[ -n "${KDKROOT:-}" ] || dxb_die "KDKROOT is not set: set it to your unpacked Kernel Debug Kit 26.5 (25F71; Kernel_Debug_Kit_26.5_build_25F71.dmg, pkgutil --expand-full). It is Apple's and not in this repository"
	[ -d "$KDKROOT" ] || dxb_die "the Kernel Debug Kit is missing at KDKROOT=$KDKROOT (Kernel_Debug_Kit_26.5_build_25F71.dmg, pkgutil --expand-full)"
	local tool
	for tool in jq gum xcodes cmake ninja git python3; do
		command -v "$tool" >/dev/null || dxb_die "missing $tool: darwin-xnu-build needs jq, gum, xcodes, cmake and ninja on PATH (TOOLS_BIN=<directory> adds one)"
	done
	export DEVELOPER_DIR KDKROOT
	export MACOS_VERSION=26.5 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT}
	export MACHINE_CONFIG=${MACHINE_CONFIG:-$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get machine)}
	export SKIP_PATCHES=1
}

# dxb_cfg: the kernel's name in dxb's directories, <config>.<machine> in lower case (kernel.<...>; the iOS-target kernel is mach.<...>)
dxb_cfg() { printf '%s.%s' "$(echo "$KERNEL_CONFIG" | tr '[:upper:]' '[:lower:]')" "$(echo "$MACHINE_CONFIG" | tr '[:upper:]' '[:lower:]')"; }

# dxb_tree_id: what the kernel tree in build/dxb-12377/xnu was made from: the fork's commit, the stage scripts and the copy list of
# the profile. Compared with .series-12377 to see that the tree is still the profile's.
dxb_tree_id() {
	local fc stage s
	fc=$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get fork_commit 2>/dev/null || true)
	stage=$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get fork_stage 2>/dev/null || true)
	{ echo "fork $fc"; for s in $stage; do cat "$ROOT/$s"; done; } | shasum -a 256 | cut -c1-16
}

# dxb_make_tree: dxb at its pinned commit, edited, with the profile's tree in it
# (called after `||`, where `set -e` does not act: every step says what it does on failure)
dxb_make_tree() {
	rm -rf "$DXB"
	git init -q "$DXB" || return 1
	git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA" || return 1
	git -C "$DXB" checkout -q FETCH_HEAD || return 1
	[ "$(git -C "$DXB" rev-parse HEAD)" = "$DXB_SHA" ] || { echo "$DXB_REPO gave $(git -C "$DXB" rev-parse HEAD), expected $DXB_SHA" >&2; return 1; }
	# Same firehose flags as the iOS 16 build: plain arm64, no ARMv8.1 LSE atomics.
	python3 - "$DXB/build.sh" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
old = 'ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"'
new = ('ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc '
       '-Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto"')
text = path.read_text()
assert text.count(old) == 1, 'darwin-xnu-build firehose line changed'
text = text.replace(old, new).replace('make install -j8', 'make install -j4')
path.write_text(text)
PY
	[ $? -eq 0 ] || return 1
	# The tree: the profile's (the fork's commit). Its stage scripts fetch
	# Apple's HFS and libpthread and BearSSL by commit and take CoreEntitlements' headers from the Kernel Debug Kit.
	OS_PROFILE=$OS_PROFILE "$ROOT/xnu/apply-series.sh" "$DXB/xnu" || return 1
	grep -q 'SKIP_PATCHES' "$DXB/build.sh" ||
		sed -i '' 's/^    patches$/    [ -n "${SKIP_PATCHES:-}" ] || patches/' "$DXB/build.sh" || return 1
	dxb_tree_id > "$DXB/.series-12377"
}

# dxb_copies: the repository's own files the tree takes (the profile's fork_copy: shared drivers that live here, not in the fork)
# are put there again when they have changed; the new copy gets a new time so that make sees it (what xnu/sync-drivers.sh does
# for the other profiles)
dxb_copies() {
	local list src dst
	list=$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get fork_copy 2>/dev/null || true)
	[ -n "$list" ] || return 0
	grep -v '^#' "$ROOT/$list" | while read -r src dst; do
		[ -n "$src" ] || continue
		cmp -s "$ROOT/$src" "$DXB/xnu/$dst" || { cp "$ROOT/$src" "$DXB/xnu/$dst" && echo "$DXB_WHO: $dst is $src again"; }
	done
}

# dxb_setup: build/dxb-12377 made (FRESH=1: again), or checked to be the profile's
dxb_setup() {
	if [ ! -d "$DXB" ] || [ -n "${FRESH:-}" ]; then
		# a half-made directory is no use: it is removed, and the next run starts again
		dxb_make_tree || { rm -rf "$DXB"; dxb_die "setting up $DXB failed (removed)"; }
	else
		# The tree is made once, when the directory is. make is incremental afterwards; a changed profile needs a fresh tree.
		[ "$(cat "$DXB/.series-12377" 2>/dev/null)" = "$(dxb_tree_id)" ] ||
			dxb_die "the profile's tree changed (or $DXB was not made from it) since $DXB was set up: run with FRESH=1"
		dxb_copies
	fi
}

# dxb_kernel_ios: where the iOS-target build leaves the kernel (mach.<config>.<machine>, not kernel.*), in dxb's own object directory
dxb_kernel_ios() {
	local config=${KERNEL_CONFIG:-DEVELOPMENT} machine=${MACHINE_CONFIG:-$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get machine)}
	printf '%s/build/xnu-ios.obj/%s_ARM64_%s/mach.%s.%s\n' "$DXB" "$config" "$machine" \
		"$(echo "$config" | tr '[:upper:]' '[:lower:]')" "$(echo "$machine" | tr '[:upper:]' '[:lower:]')"
}
