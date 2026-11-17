#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The Exynos video decoder's checks on what a client's stream makes the firmware report and on a client's zero-copy surface
# (drivers/media/mfc/hakios.mfc_check.h, used by hakios.exynos_mfc.c) on this machine: tools/mfc_check_test.c built against
# it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/mfc-check-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/mfc_check_test.c -o "$tmp/mfc_check_test"
"$tmp/mfc_check_test"
