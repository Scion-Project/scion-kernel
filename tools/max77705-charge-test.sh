#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The Galaxy S9's charging rules (drivers/power/max77705/hakios.max77705_charge.h: the iOS 27 charger guard, sweetchg=2, and
# its fast-charge path, sweetafc=1) on this machine: tools/max77705_charge_test.c built against them under the address and
# undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/max77705-charge-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/max77705_charge_test.c \
    -o "$tmp/max77705_charge_test"
"$tmp/max77705_charge_test"
