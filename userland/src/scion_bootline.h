/* SPDX-License-Identifier: MIT-0 */
/*
 * scion_bootline.h: the kernel's boot line (sysctl kern.bootargs), whole, and its arguments, each read exactly.
 *
 * Why it exists. The Galaxy S9's kernel builds its boot line in PE_BOOT_ARGS_LENGTH (2048) bytes: the loader's line
 * and the built-in defaults after it (the fork's osfmk/arm/arm_init.c, starlte_default_bootargs) come to about
 * 1.1-1.2 KB. kern.bootargs does not cut a line that does not fit the buffer it is given: it refuses (ENOMEM) and gives
 * nothing (trunc 0, the fork's bsd/kern/kern_sysctl.c; launchd_cache_loader relies on that). Every reader of this port
 * read the line into a fixed 1 KB buffer (or less), so on the S9 every switch they look for was silently not seen and
 * each fell back to its default; and they looked with strstr(), so "foo=1" also matched "xfoo=1" and "foo=10".
 * Everything that reads the line now does it here:
 *
 *   scion_boot_line()                     the whole line (asked for its size first; read once, kept); "" when unreadable
 *   scion_boot_arg(NAME, &length)         the value of the word named NAME (NAME=VALUE): a pointer into the line and
 *                                         its length (up to the next blank); "" (length 0) for a bare word NAME or
 *                                         NAME=; NULL when no word is named NAME
 *   scion_boot_arg_set(NAME)              a word named NAME is on the line, whatever its value
 *   scion_boot_arg_is(NAME, VALUE)        a word NAME=VALUE is on the line, VALUE whole
 *   scion_boot_arg_number(NAME, DEFAULT)  NAME's value as a number (as strtoll, base 0: 10, 0x16, 017), DEFAULT when
 *                                         there is no such word or its value is not wholly a number
 *   scion_boot_arg_copy(NAME, BUF, SIZE)  NAME's value into BUF (cut to SIZE - 1, NUL-terminated); 1 when NAME is there
 *
 * A word's name is what stands before its first '=' (the whole word when it has none: "-v", "-n"); words are separated
 * by blanks (space, tab). The first word with the name wins, as in the kernel's PE_parse_boot_argn (the loader's line
 * comes first, and a built-in default the loader's line names is not appended at all). The *_in(LINE, ...) forms do
 * the same on a line given, for a test or for a line read some other way.
 *
 * Plain C (C99) and header only: each program and library of the port includes it where it reads the line, so no build
 * has a file to add. tools/bootline_test.c is its host test (tools/bootline-test.sh, CI). The name of each argument is
 * the literal at the call, which tools/boot-args.py reads for the catalogue (docs/boot-args.md).
 */
#ifndef SCION_BOOTLINE_H
#define SCION_BOOTLINE_H

/*
 * Two builds: with the C library (the default; the line in memory of its own), and inside libmini's static programs
 * (init, var_store: userland/libmini, included first), which have no allocator: the line in a buffer of the kernel's
 * longest line.
 */
#ifdef MINI_H
#define SCION_BOOTLINE_STATIC 1
#define SCION_BOOTLINE_ENOMEM 12
#else
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define SCION_BOOTLINE_ENOMEM ENOMEM
#endif

/* the longest line the kernel has (pexpert.h PE_BOOT_ARGS_LENGTH on the S9, NUL included); with the C library a line
 * is read whole even when the kernel says it is longer */
#define SCION_BOOT_LINE_MAX 2048

#ifndef SCION_BOOTLINE_SYSCTLBYNAME
#ifndef MINI_H
#include <sys/sysctl.h>
#endif
#define SCION_BOOTLINE_SYSCTLBYNAME sysctlbyname
#endif

static inline int
scion_bootline_blank(char c)
{
	return c == ' ' || c == '\t';
}

/* the value of the first word of LINE named NAME (see above), or NULL */
static inline const char *
scion_boot_arg_in(const char *line, const char *name, size_t *length)
{
	size_t n = name != NULL ? strlen(name) : 0;
	const char *p = line != NULL ? line : "";

	if (n == 0) {
		return NULL;
	}
	while (*p != '\0') {
		while (scion_bootline_blank(*p)) {
			p++;
		}
		const char *word = p;
		while (*p != '\0' && !scion_bootline_blank(*p) && *p != '=') {
			p++;
		}
		int named = (size_t)(p - word) == n && strncmp(word, name, n) == 0;
		const char *value = *p == '=' ? p + 1 : p;
		while (*p != '\0' && !scion_bootline_blank(*p)) {
			p++;
		}
		if (named) {
			if (length != NULL) {
				*length = (size_t)(p - value);
			}
			return value;
		}
	}
	return NULL;
}

static inline int
scion_boot_arg_set_in(const char *line, const char *name)
{
	return scion_boot_arg_in(line, name, NULL) != NULL;
}

static inline int
scion_boot_arg_is_in(const char *line, const char *name, const char *value)
{
	size_t length = 0;
	const char *v = scion_boot_arg_in(line, name, &length);
	return v != NULL && value != NULL && strlen(value) == length && strncmp(v, value, length) == 0;
}

static inline long long
scion_boot_arg_number_in(const char *line, const char *name, long long fallback)
{
	size_t length = 0, i = 0;
	const char *v = scion_boot_arg_in(line, name, &length);
	unsigned long long number = 0, base = 10, limit;
	int negative = 0;

	if (v == NULL) {
		return fallback;
	}
	if (i < length && (v[i] == '-' || v[i] == '+')) {
		negative = v[i++] == '-';
	}
	if (i + 1 < length && v[i] == '0' && (v[i + 1] == 'x' || v[i + 1] == 'X')) {
		base = 16;
		i += 2;
	} else if (i + 1 < length && v[i] == '0') {
		base = 8;
		i++;
	}
	if (i >= length) {
		return fallback;
	}
	limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
	for (; i < length; i++) {
		char c = v[i];
		unsigned long long d = c >= '0' && c <= '9' ? (unsigned long long)(c - '0') :
		    c >= 'a' && c <= 'f' ? (unsigned long long)(c - 'a' + 10) :
		    c >= 'A' && c <= 'F' ? (unsigned long long)(c - 'A' + 10) : 99;
		if (d >= base || number > (limit - d) / base) {
			return fallback;
		}
		number = number * base + d;
	}
	return negative ? (long long)(0 - number) : (long long)number;
}

static inline int
scion_boot_arg_copy_in(const char *line, const char *name, char *buffer, size_t size)
{
	size_t length = 0;
	const char *v = scion_boot_arg_in(line, name, &length);

	if (v == NULL || buffer == NULL || size == 0) {
		return v != NULL;
	}
	if (length > size - 1) {
		length = size - 1;
	}
	memcpy(buffer, v, length);
	buffer[length] = '\0';
	return 1;
}

#ifdef SCION_BOOTLINE_STATIC
/* the line, whole, into a buffer of the kernel's longest line; NULL when it cannot be read or is longer */
static inline char *
scion_boot_line_read(void)
{
	static char line[SCION_BOOT_LINE_MAX + 1];
	size_t got = SCION_BOOT_LINE_MAX;

	if (SCION_BOOTLINE_SYSCTLBYNAME("kern.bootargs", line, &got, NULL, 0) != 0) {
		return NULL;
	}
	line[got < SCION_BOOT_LINE_MAX ? got : SCION_BOOT_LINE_MAX] = '\0';
	return line;
}
#define SCION_BOOTLINE_FREE(line) ((void)(line))
#else
/* the line, read whole into memory of its own (malloc), or NULL */
static inline char *
scion_boot_line_read(void)
{
	size_t tried = 0;

	for (int attempt = 0; attempt < 4; attempt++) {
		size_t size = 0;
		if (SCION_BOOTLINE_SYSCTLBYNAME("kern.bootargs", NULL, &size, NULL, 0) != 0) {
			size = 0;
		}
		if (size < SCION_BOOT_LINE_MAX) {
			size = SCION_BOOT_LINE_MAX;
		}
		if (size <= tried) {
			size = tried * 2;       /* refused at that size already: the size said is short */
		}
		tried = size;
		char *line = (char *)calloc(1, size + 1);
		size_t got = size;
		if (line == NULL) {
			return NULL;
		}
		if (SCION_BOOTLINE_SYSCTLBYNAME("kern.bootargs", line, &got, NULL, 0) == 0) {
			line[got < size ? got : size] = '\0';
			return line;
		}
		int error = errno;
		free(line);
		if (error != SCION_BOOTLINE_ENOMEM) {
			return NULL;
		}
	}
	return NULL;
}
#define SCION_BOOTLINE_FREE(line) free(line)
#endif

/* the kernel's boot line, whole; read once (per program or library that includes this) and kept */
static inline const char *
scion_boot_line(void)
{
	static const char *kept;
	const char *line = __atomic_load_n(&kept, __ATOMIC_ACQUIRE);

	if (line == NULL) {
		char *fresh = scion_boot_line_read();
		const char *expected = NULL, *mine = fresh != NULL ? fresh : "";
		if (__atomic_compare_exchange_n(&kept, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			line = mine;
		} else {
			SCION_BOOTLINE_FREE(fresh);    /* another thread kept its own first */
			line = expected;
		}
	}
	return line;
}

static inline const char *
scion_boot_arg(const char *name, size_t *length)
{
	return scion_boot_arg_in(scion_boot_line(), name, length);
}

static inline int
scion_boot_arg_set(const char *name)
{
	return scion_boot_arg_set_in(scion_boot_line(), name);
}

static inline int
scion_boot_arg_is(const char *name, const char *value)
{
	return scion_boot_arg_is_in(scion_boot_line(), name, value);
}

static inline long long
scion_boot_arg_number(const char *name, long long fallback)
{
	return scion_boot_arg_number_in(scion_boot_line(), name, fallback);
}

static inline int
scion_boot_arg_copy(const char *name, char *buffer, size_t size)
{
	return scion_boot_arg_copy_in(scion_boot_line(), name, buffer, size);
}

#endif /* SCION_BOOTLINE_H */
