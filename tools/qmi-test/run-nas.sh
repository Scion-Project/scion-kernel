#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/xnu-deps/build/qmi-test/nas-test"
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$(python3 "$root/tools/xnu-tree.py" added)/bsd" -I"$root/tools/qmi-test/shim" -o "$out" \
    "$root/tools/qmi-test/test_nas.c" "$root/drivers/modem/hakios.nas.c"
"$out"
