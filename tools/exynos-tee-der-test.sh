#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The DER reader of the Galaxy S9's secure-OS probe (drivers/soc/exynos/hakios.exynos_tee_der.h, what
# drivers/soc/exynos/hakios.exynos_tee.c reads the keymaster TA's answers with) on this machine: tools/exynos_tee_der_test.c
# built against it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/exynos-tee-der-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/exynos_tee_der_test.c \
    -o "$tmp/exynos_tee_der_test"
"$tmp/exynos_tee_der_test"
