#!/bin/sh
# The boot line's reader that user space shares (userland/src/scion_bootline.h): tools/bootline_test.c built against it
# under the address and undefined-behaviour sanitizers, and run. The line is read through a stand-in for sysctl
# kern.bootargs that refuses a short buffer as the S9's kernel does; every argument is found exactly ("foo=1" is not
# "xfoo=1" nor "foo=10").
# usage: tools/bootline-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	tools/bootline_test.c -o "$tmp/bootline_test"
"$tmp/bootline_test"
# ... and as libmini's static programs (init, var_store) build it: no allocator, the kernel's longest line in a buffer
cc -std=c99 -D_DEFAULT_SOURCE -DSCION_BOOTLINE_STATIC -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	-fno-sanitize-recover=all tools/bootline_test.c -o "$tmp/bootline_test_static"
"$tmp/bootline_test_static"
# ... and it compiles with libmini's headers alone (freestanding, no C library headers): a stand-in mini.h
cat > "$tmp/mini.h" <<'H'
#ifndef MINI_H
#define MINI_H
#include <stddef.h>
#include <stdint.h>
extern int errno;
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
#endif
H
printf '#include "mini.h"\n#include "scion_bootline.h"\nint f(void) { return scion_boot_arg_is("var_store", "1") + (int)scion_boot_arg_number("n", 0); }\n' > "$tmp/m.c"
cc -std=c99 -ffreestanding -nostdinc -isystem "$(cc -print-file-name=include)" -I"$tmp" -Iuserland/src -Wall -Wextra -Werror \
	-c "$tmp/m.c" -o "$tmp/m.o"
echo "bootline_test: compiles freestanding with libmini's headers"
