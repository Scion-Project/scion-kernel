#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The Galaxy S9's modem driver (drivers/modem/hakios.shannon.c) on this machine, as far as it reads what it does not
# control: the CP's frames in the rings, its control messages and file requests, its downlink packets, the boot loader's
# queue, RADIO's table of contents. tools/modem/shannon_test.c stands in for the kernel's calls and includes the driver
# as it is, with its #include <...> lines taken out and its one secure-monitor call (arm64 assembly) replaced by the
# test's; built under the address and undefined-behaviour sanitizers, and run. Nothing talks to a phone.
# (-Wno-format: the driver prints uint64_t with %llu, as the kernel's is; on a Linux host it is unsigned long.)
# usage: tools/modem/shannon-test.sh [ITERATIONS]
set -e
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -e '/^#include </d' -e '/^smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)$/,/^}$/c\
smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3) { return test_smc(fid, a1, a2, a3); }' \
    drivers/modem/hakios.shannon.c > "$tmp/shannon_body.c"
grep -q 'return test_smc' "$tmp/shannon_body.c" || { echo "shannon-test: the driver's smc() was not found" >&2; exit 1; }
cc -O1 -g -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-function -Wno-format -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I "$tmp" -I drivers/modem tools/modem/shannon_test.c -o "$tmp/shannon_test"
"$tmp/shannon_test" "$@"
