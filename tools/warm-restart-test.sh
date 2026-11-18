#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The warm restart's re-entry record (docs/ios27/WARM-RESTART.md): xnuloader/sboot/warm_record.c, the loader's checks
# of the record a running kernel leaves it and of the payload's SHA-256, built for the host with tools/warm_restart_test.c
# under the address and undefined-behaviour sanitizers, and run.
# usage: tools/warm-restart-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$tmp/warm-restart-test" tools/warm_restart_test.c xnuloader/sboot/warm_record.c
"$tmp/warm-restart-test"
