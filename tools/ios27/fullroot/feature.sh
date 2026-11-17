# SPDX-License-Identifier: MIT-0
# shellcheck shell=bash
# feature.sh : sourced by the S9 feature steps of the root build (steps.d/9N-*.sh), never run by itself. It gives every
# feature step the same rules (docs/ios27/INSTALL-S9.md, "The S9's features in the root"):
#   - ENABLE_<VAR>=0 leaves the feature out; it is on by default (a step may say "auto" and decide from the root).
#   - An input only the builder can supply is missing: one line naming the variable to set, and the root is built
#     without the feature (feature_skip). Firmware is not such an input: the device's firmware is fetched and checked by
#     firmware.sh (feature_firmware), and when it cannot be had the build stops instead (feature_die).
#   - The feature's programs are built from this checkout with the builder's Xcode (DEVELOPER_DIR), as the builder's own
#     user when the root build runs under sudo (feature_build), into build/ios27-features/<name>/ (not in git).
#   - The step's work runs in a child process; if it fails, the step's own undo runs, so the root holds either the whole
#     feature or none of it, and the build goes on; STRICT=1 makes a failed feature stop the build (and so does an undo
#     that fails: the root could then hold part of the feature).
#   - A library a feature has the kernel insert into a program (sweetinject=) is recorded in the root, one file per
#     library under /usr/local/share/scion/sweetinject.d/ (feature_inject); inject-record.sh turns them into the record
#     next to the image that make-boot-image.sh reads, so the boot image never names a library the root does not have.
# A step defines feature_apply and feature_undo (and feature_inputs when it has inputs to check), then calls feature_main.
# The step's name (FEATURE) prefixes every line it prints.

FEATURE_SK=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
FEATURE_INJECT_DIR=usr/local/share/scion/sweetinject.d

feature_say() { echo "$FEATURE: $*"; }
feature_die() { echo "$FEATURE: $*" >&2; exit 1; }
feature_skip() { echo "$FEATURE: skipped, the root is built without it: $*"; exit 0; }

# feature_setup NAME VAR : NAME is the step's file name without .sh, VAR the suffix of ENABLE_<VAR>
feature_setup() {
	FEATURE=$1; FEATURE_VAR=ENABLE_$2
	FEATURE_SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
	FEATURE_OUT=${FEATURE_BUILD_DIR:-$FEATURE_SK/build/ios27-features}/${FEATURE#[0-9][0-9]-}
}

# the device's vendor firmware (fw_prepare, FIRMWARE_DIR, fw_check_kind): firmware.sh, shared with the root build and the boot image
. "$(dirname "${BASH_SOURCE[0]}")/firmware.sh"

# feature_as_builder CMD... : CMD as the builder's own user when the build runs under sudo (no root-owned files in the
# checkout), with DEVELOPER_DIR and PY passed through; as the caller otherwise (firmware.sh's fw_as_builder)
feature_as_builder() { fw_as_builder "$@"; }

# feature_firmware KIND WHAT : the device's firmware made complete and checked (firmware.sh), or the build stopped: a feature
# that is on is never built without its firmware (ENABLE_<VAR>=0 leaves it out on purpose)
feature_firmware() {
	fw_prepare || feature_die "the $2 is not complete (above): the build stops ($FEATURE_VAR=0 builds the root without it)"
	[[ -f $FIRMWARE_DIR/$1/SHA256SUMS ]] || feature_die "$FIRMWARE_DIR has no $1/SHA256SUMS"
}

# feature_have_sdk : is there an Xcode with the iPhoneOS SDK for the builder?
feature_have_sdk() { feature_as_builder xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; }

# feature_build CMD... : run a build script as the builder, its output into $FEATURE_OUT (made fresh)
feature_build() {
	feature_as_builder mkdir -p "$(dirname "$FEATURE_OUT")"
	feature_as_builder rm -rf "$FEATURE_OUT"
	feature_as_builder mkdir -p "$FEATURE_OUT"
	feature_as_builder "$@"
}

# feature_inject LIBRARY PROGRAM... : record that LIBRARY (a path on the phone) is to be inserted into PROGRAMs
feature_inject() {
	local lib=$1; shift
	[[ -f $R$lib ]] || feature_die "feature_inject: $lib is not on the root"
	install -d -m 0755 "$R/$FEATURE_INJECT_DIR"
	printf '%s %s\n' "$lib" "$*" > "$R/$FEATURE_INJECT_DIR/$(basename "$lib")"
	[[ $EUID != 0 ]] || chown 0:0 "$R/$FEATURE_INJECT_DIR/$(basename "$lib")" "$R/$FEATURE_INJECT_DIR"
}
feature_uninject() { rm -f "$R/$FEATURE_INJECT_DIR/$(basename "$1")"; rmdir "$R/$FEATURE_INJECT_DIR" "$R/usr/local/share/scion" "$R/usr/local/share" 2>/dev/null || true; }

# feature_put SRC DST MODE : one file into the root, replaced whole, root:wheel when run as root
feature_put() {
	install -d -m 0755 "$(dirname "$2")"
	cat "$1" > "$2.new.$$" && chmod "$3" "$2.new.$$" && mv -f "$2.new.$$" "$2"
	[[ $EUID != 0 ]] || chown 0:0 "$2"
}

# feature_main ROOT [apply|undo] : the step's entry point (the step's own arguments, unchanged)
feature_main() {
	R=${1:?usage: $FEATURE.sh ROOT [undo]}; local how=${2:-apply}
	[[ -f $R/System/Library/xpc/launchd.plist && -d $R/private/var ]] || feature_die "$R is not an iOS root"
	export R
	case $how in
	undo) feature_undo; exit 0 ;;
	inner) feature_apply; exit 0 ;;            # the child process of "apply" below
	apply) ;;
	*) feature_die "what is '$how'? (nothing, or undo)" ;;
	esac
	local on=${!FEATURE_VAR:-1}
	[[ $on == 0 ]] && { feature_say "off ($FEATURE_VAR=0)"; exit 0; }
	! declare -F feature_inputs >/dev/null || feature_inputs
	# a fresh bash: errexit holds in it (it would not inside a subshell tested with ||)
	if bash "$FEATURE_SELF" "$R" inner; then
		exit 0
	fi
	echo "$FEATURE: FAILED: undoing it, the root is built without it$([[ ${STRICT:-0} == 1 ]] && echo ' (STRICT=1: the build stops)')" >&2
	bash "$FEATURE_SELF" "$R" undo >&2 || { echo "$FEATURE: the undo failed too: the root may hold part of it, the build stops" >&2; exit 1; }
	[[ ${STRICT:-0} == 1 ]] && exit 1
	exit 0
}
