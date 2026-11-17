#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The UFS driver's commands and the whitelist every one passes before it reaches the flash
# (drivers/storage/ufs/hakios.ufshc_upiu.h, what drivers/storage/ufs/hakios.ufshc.c builds and checks its commands with) on
# this machine: tools/ufshc_upiu_test.c built against it under the address and undefined-behaviour sanitizers, and run.
# The header is plain C.
# usage: tools/ufshc-upiu-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/ufshc_upiu_test.c \
    -o "$tmp/ufshc_upiu_test"
"$tmp/ufshc_upiu_test"
