#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The rule for the Galaxy S9's M3s above 1794 MHz (drivers/power/exynos/hakios.exynos_cpufreq_ceiling.h, what every option of
# drivers/power/exynos/hakios.exynos_cpufreq.c and the fork's copy go through) on this machine: tools/exynos_cpufreq_ceiling_test.c
# built against it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/exynos-cpufreq-ceiling-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/exynos_cpufreq_ceiling_test.c \
    -o "$tmp/exynos_cpufreq_ceiling_test"
"$tmp/exynos_cpufreq_ceiling_test"
