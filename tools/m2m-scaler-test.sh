#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The S9's memory-to-memory scaler (drivers/media/m2m/hakios.m2m_core.h: the pixels of the AppleM2ScalerCSCDriver service
# that iOS's IOSurfaceAccelerator opens) on this machine: tools/m2m/m2m_test.c built against it under the address and
# undefined-behaviour sanitizers, and run: its transfers against double-precision reference pictures, the exact answers
# of turns and flips, the range conversions, and the published limits as the edges of what it accepts.
# usage: tools/m2m-scaler-test.sh            (M2M_TEST_PGM=dir also writes the pictures as PGM files)
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/m2m/m2m_test.c -lm \
    -o "$tmp/m2m_test"
"$tmp/m2m_test"
