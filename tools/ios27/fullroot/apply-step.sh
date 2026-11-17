#!/bin/bash
# apply-step.sh IMG STEP.sh [VAR=value ...] [undo] : (root) attach IMG read-write with owners, run one steps.d script on it (with
# undo: the step's undo, for the steps that have one), fsck, detach. For re-running a single step on a built image (steps are
# idempotent); the image is modified in place - clone it first (cp -c). IMG.inject, the record of the libraries the root has
# for sweetinject= (inject-record.sh), is made again from the root afterwards: make the boot image again with it.
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root" >&2; exit 2; }
IMG=$1; STEP=$2; shift 2
ARG=; [[ ${!#:-} == undo ]] && { ARG=undo; set -- "${@:1:$(($# - 1))}"; }
H=$(cd "$(dirname "$0")" && pwd); M=$(mktemp -d /tmp/applystep.XXXX)
DEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" | awk 'NR==1 {print $1}')
trap 'umount "$M" 2>/dev/null; hdiutil detach "$DEV" >/dev/null 2>&1; rmdir "$M" 2>/dev/null' EXIT
mount -t hfs -o owners "$DEV" "$M"
env "$@" bash "$STEP" "$M" $ARG
bash "$H/inject-record.sh" "$M" "$IMG.inject"; [[ -z ${SUDO_USER:-} ]] || chown "$SUDO_USER" "$IMG.inject"
sync; umount "$M"; fsck_hfs -n -f "/dev/r${DEV#/dev/}" 2>&1 | tail -2
