#!/bin/bash
# SPDX-License-Identifier: MIT-0
# prepare.sh : the inputs of tools/ios27/fullroot/steps.d/60-sileo.sh, made before the root build (no root; network
# for two downloads, once): the Procursus bootstrap and Sileo alone, both pinned by SHA-256 in tools/mk-jb.sh
# (PREINSTALL= ELLEKIT=: no OpenSSH, no tweak injection, nothing "newest"), and jb_seed and var_sync for iOS.
# Needs zstd, python3, Xcode's iPhoneOS SDK. Everything goes to jb/ (JB_ROOT), which is never in the repository.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
command -v zstd >/dev/null || { echo "prepare.sh: zstd is needed (brew install zstd)" >&2; exit 1; }
PREINSTALL= ELLEKIT= "$REPO/tools/mk-jb.sh"
"$REPO/tools/ios27/sileo/build-tools.sh"
