#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The framebuffer stand-in's sweetfb_nowait present state machine (drivers/display/sweetfb/hakios.sweetfb_present.h, which the
# ios-27 kernel's iokit/Kernel/arm/SweetFramebuffer.cpp takes through profiles/ios-27.copy) on this machine:
# tools/sweetfb_present_test.c drives it through random interleavings of swaps, GPU completions, stalls, vsync ticks and the
# display going off and on, under the address and undefined-behaviour sanitizers. The header is plain C.
# usage: tools/sweetfb-present-test.sh [seeds]
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/sweetfb_present_test.c -o "$tmp/sweetfb_present_test"
"$tmp/sweetfb_present_test" "${1:-300}"
