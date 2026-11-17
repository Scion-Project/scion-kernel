#!/bin/sh
# SPDX-License-Identifier: MIT-0
# The parsers of the Galaxy S9's Bluetooth driver that read what the chip sends and are not in the plain-C header: the
# scan's events (inquiry results, LE advertising reports, the names in their data) and bt_trace's H4 reader (the vendor
# commands' filter is hakios.bt_vsc.h, tools/bt-vsc-test.sh). They are taken out of
# drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c as they stand, by the lines that begin and end them, and built with
# tools/bcm_bt_parsers_test.c (which stands in for the kernel's few functions they call) under the address and
# undefined-behaviour sanitizers, and run. If the driver's lines move so that a piece is not found, this fails.
# usage: tools/bcm-bt-parsers-test.sh
set -e
cd "$(dirname "$0")/.."
src=drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# piece FROM TO: the lines from the one that begins with FROM up to the one before the one that begins with TO
piece() {
	awk -v from="$1" -v to="$2" '
		!on && index($0, from) == 1 { on = 1; found = 1 }
		on && index($0, to) == 1 { on = 0; ended = 1 }
		on { print }
		END { if (!found || !ended) { print "piece not found: " from > "/dev/stderr"; exit 1 } }' "$src"
}
{
	grep '^#define SCAN_MAX' "$src"
	piece 'struct seen {' '/* the UART'"'"'s rate, once what it is sending has gone */'
	piece '/* a name from EIR or advertising data' '/* read events for up to ms'
	piece 'static uint32_t trace_per_s;' 'static void vsc_reset(void);'
} > "$tmp/bt_pieces.h"
grep -q 'scan_event(const uint8_t \*event)' "$tmp/bt_pieces.h"
grep -q '^trace_bytes(struct h4_trace \*t' "$tmp/bt_pieces.h"
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I"$tmp" tools/bcm_bt_parsers_test.c \
    -o "$tmp/bcm_bt_parsers_test"
"$tmp/bcm_bt_parsers_test"
