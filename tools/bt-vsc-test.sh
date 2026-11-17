#!/bin/sh
# SPDX-License-Identifier: MIT-0
# drivers/bluetooth/bcm4361/hakios.bt_vsc.h, the table and filter for the vendor commands of iOS's bluetoothd on the Galaxy
# S9's BCM4361 (bt_vsc=), with tools/bt_vsc_test.c built against it under the address and undefined-behaviour sanitizers,
# and run. The header is plain C.
# usage: tools/bt-vsc-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/bt_vsc_test.c -o "$tmp/bt_vsc_test"
"$tmp/bt_vsc_test"
