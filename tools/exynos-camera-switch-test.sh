#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The Galaxy S9's camera switch (drivers/camera/exynos/hakios.exynos_camera_switch.h: what hakios.exynos_camera.c decides a
# change between the front and the rear camera with, under cam_switch=1) on this machine: tools/exynos_camera_switch_test.c
# built against it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/exynos-camera-switch-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/exynos_camera_switch_test.c \
    -o "$tmp/exynos_camera_switch_test"
"$tmp/exynos_camera_switch_test"
