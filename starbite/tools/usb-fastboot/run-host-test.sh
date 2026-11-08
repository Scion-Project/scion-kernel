#!/bin/sh
# Build and run the host test of the loader's fastboot protocol, console and USB descriptors
# (tools/usb-fastboot/host_test.c). Any C compiler: the Mac's clang or the Lima VM's gcc.
set -eu
cd "$(dirname "$0")"
S=../../shim/starlte
out=${TMPDIR:-/tmp}/starbite-fb-host-test
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$S" \
    host_test.c "$S/fastboot.c" "$S/usb_desc.c" -o "$out"
"$out"
