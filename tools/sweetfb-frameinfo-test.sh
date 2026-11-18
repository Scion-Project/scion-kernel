#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The framebuffer stand-in's frame-info answers (drivers/display/sweetfb/hakios.sweetfb_frameinfo.h, which the ios-27
# kernel's iokit/Kernel/arm/SweetFramebuffer.cpp takes through profiles/ios-27.copy) on this machine:
# tools/sweetfb_frameinfo_test.c reads the key list and decodes every notification as iOS 27.0.1's IOMobileFramebuffer
# does, for every boot-argument value, mask and known value, under the address and undefined-behaviour sanitizers.
# The header is plain C.
# usage: tools/sweetfb-frameinfo-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/sweetfb_frameinfo_test.c \
    -o "$tmp/sweetfb_frameinfo_test"
"$tmp/sweetfb_frameinfo_test"
