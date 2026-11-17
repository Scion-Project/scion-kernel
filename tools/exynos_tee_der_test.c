/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/exynos_tee_der_test.c: drivers/soc/exynos/hakios.exynos_tee_der.h on a computer, under the address and
 * undefined-behaviour sanitizers (tools/exynos-tee-der-test.sh). der_next() and der_read_integer() read the answers of
 * Samsung's keymaster TA, which come from the secure world through shared memory.
 *
 * What is checked:
 *   - TLVs made here, short and long form (one, two, three length bytes), each read back with its tag, contents and
 *     length, and *at left just past it; a run of them read one after another to the end;
 *   - what must be refused: a header cut short, a long form of zero or more than three bytes, a length byte count past
 *     the end, contents past the end by one, a TLV that starts at the limit;
 *   - INTEGERs: positive, negative (sign-extended), the 0x00 that keeps a high bit positive, nine bytes, none;
 *   - 2,000,000 buffers made from good ones by a fixed-seed mutator, each held in exactly its own length on the heap so
 *     that the sanitizer stops any read past it, walked to the end with der_next as hakios.exynos_tee.c walks an answer
 *     (nested once, as km_parse does): whatever it returns lies inside the buffer, and the walk always ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/soc/exynos/hakios.exynos_tee_der.h"

static unsigned long checks, failures;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x2545f4914f6cdd1dull;

static uint64_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* a TLV of tag with n bytes of contents (each the byte fill) into out, in the shortest form or with lb length bytes */
static uint32_t
put_tlv(uint8_t *out, uint8_t tag, uint32_t n, uint8_t fill, int lb)
{
	uint32_t h = 0;

	out[h++] = tag;
	if (lb == 0) {
		out[h++] = (uint8_t)n;
	} else {
		out[h++] = (uint8_t)(0x80 | lb);
		for (int i = lb - 1; i >= 0; i--) {
			out[h++] = (uint8_t)(n >> (8 * i));
		}
	}
	memset(out + h, fill, n);
	return h + n;
}

/* the bytes of p, n of them, in a heap buffer of exactly n bytes */
static uint8_t *
exact(const uint8_t *p, uint32_t n)
{
	uint8_t *b = malloc(n ? n : 1);

	memcpy(b, p, n);
	return b;
}

static void
test_good(void)
{
	static const struct { uint32_t n; int lb; } cases[] = {
		{ 0, 0 }, { 1, 0 }, { 127, 0 }, { 0, 1 }, { 128, 1 }, { 255, 1 }, { 256, 2 }, { 1000, 2 }, { 65535, 2 },
		{ 65536, 3 }, { 70000, 3 },
	};
	static uint8_t buf[80000];

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t total = put_tlv(buf, 0x30, cases[i].n, 0x5a, cases[i].lb);
		uint8_t *b = exact(buf, total);
		uint32_t at = 0, c = 0, l = 0;
		uint8_t tag = 0;

		CHECK(der_next(b, total, &at, &tag, &c, &l), "a TLV of %u bytes (%d length bytes) is refused", cases[i].n,
		    cases[i].lb);
		CHECK(tag == 0x30 && l == cases[i].n && c == total - cases[i].n && at == total,
		    "a TLV of %u bytes: tag %02x, contents at %u, length %u, next at %u", cases[i].n, tag, c, l, at);
		/* one byte short: refused */
		at = 0;
		CHECK(!der_next(b, total - 1, &at, &tag, &c, &l), "a TLV of %u bytes, one byte cut off, is taken", cases[i].n);
		free(b);
	}
	/* a run of TLVs to the end */
	uint32_t n = 0, count = 0;
	for (int k = 0; k < 50; k++) {
		uint32_t len = (uint32_t)(k * 7 % 300);

		n += put_tlv(buf + n, (uint8_t)(0x80 | k), len, (uint8_t)k, k % 4 == 3 || len >= 256 ? 2 : len >= 128 ? 1 : 0);
	}
	uint8_t *b = exact(buf, n);
	uint32_t at = 0, c, l;
	uint8_t tag;
	while (at < n && der_next(b, n, &at, &tag, &c, &l)) {
		CHECK(tag == (uint8_t)(0x80 | count) && l == count * 7 % 300 && c + l == at, "TLV %u of the run", count);
		count++;
	}
	CHECK(count == 50 && at == n, "a run of 50 TLVs read as %u, ending at %u of %u", count, at, n);
	free(b);
}

static void
test_refused(void)
{
	static const struct { uint8_t bytes[8]; uint32_t n; const char *what; } bad[] = {
		{ { 0x30 }, 1, "a header of one byte" },
		{ { 0x30, 0x80 }, 2, "a long form of zero bytes" },
		{ { 0x30, 0x84, 0, 0, 0, 1, 0 }, 7, "a long form of four bytes" },
		{ { 0x30, 0xff }, 2, "a long form of 127 bytes" },
		{ { 0x30, 0x82, 0x01 }, 3, "a long form cut short" },
		{ { 0x30, 0x02, 0x01 }, 3, "contents past the end by one" },
		{ { 0x30, 0x81, 0x05, 1, 2, 3, 4 }, 7, "long-form contents past the end by one" },
		{ { 0x30, 0x83, 0xff, 0xff, 0xff, 0 }, 6, "a length of 2^24 - 1 in a short buffer" },
	};

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint8_t *b = exact(bad[i].bytes, bad[i].n);
		uint32_t at = 0, c = 0xdead, l = 0xbeef;
		uint8_t tag = 0;

		CHECK(!der_next(b, bad[i].n, &at, &tag, &c, &l), "%s is taken", bad[i].what);
		CHECK(at == 0, "%s moves *at to %u", bad[i].what, at);
		free(b);
	}
	uint8_t two[2] = { 0x04, 0x00 };
	uint8_t *b = exact(two, 2);
	uint32_t at = 2, c, l;
	uint8_t tag;
	CHECK(!der_next(b, 2, &at, &tag, &c, &l), "a TLV that starts at the limit is taken");
	free(b);
}

static void
test_integers(void)
{
	static const struct { uint8_t bytes[9]; uint32_t n; int64_t want; } ints[] = {
		{ { 0x00 }, 1, 0 },
		{ { 0x7f }, 1, 127 },
		{ { 0x80 }, 1, -128 },
		{ { 0xff }, 1, -1 },
		{ { 0x00, 0x80 }, 2, 128 },
		{ { 0x01, 0x00 }, 2, 256 },
		{ { 0xff, 0x7f }, 2, -129 },
		{ { 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 8, INT64_MAX },
		{ { 0x80, 0, 0, 0, 0, 0, 0, 0 }, 8, INT64_MIN },
		{ { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 9, -1 },     /* 2^64 - 1 as the u64 it is */
		{ { 0 }, 0, 0 },
	};

	for (unsigned i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
		uint8_t *b = exact(ints[i].bytes, ints[i].n);
		int64_t v = der_read_integer(b, 0, ints[i].n);

		CHECK(v == ints[i].want, "INTEGER %u: %lld, not %lld", i, (long long)v, (long long)ints[i].want);
		free(b);
	}
}

/* as km_parse walks an answer: the outer TLVs to the end, and one level inside each */
static void
walk(const uint8_t *b, uint32_t n)
{
	uint32_t at = 0, steps = 0, c, l;
	uint8_t tag;

	while (at < n) {
		uint32_t was = at;

		if (!der_next(b, n, &at, &tag, &c, &l)) {
			break;
		}
		CHECK(c >= was + 2 && c <= n && l <= n - c && at == c + l && at > was, "a TLV outside the buffer: at %u, "
		    "contents %u + %u of %u", was, c, l, n);
		uint32_t inner = c, c2, l2;
		uint8_t t2;
		while (inner < c + l && der_next(b, c + l, &inner, &t2, &c2, &l2)) {
			CHECK(c2 + l2 <= c + l, "an inner TLV past its outer one");
			if (t2 == 0x02) {
				(void)der_read_integer(b, c2, l2);
			}
		}
		if (tag == 0x02) {
			(void)der_read_integer(b, c, l);
		}
		CHECK(++steps <= n, "the walk does not end");
	}
}

static void
test_mutated(void)
{
	static uint8_t seed[600];
	uint32_t n = 0;

	/* an answer of the shape km_parse reads: a SEQUENCE of INTEGERs and context-tagged fields */
	uint8_t inner[500];
	uint32_t m = 0;
	m += put_tlv(inner + m, 0x02, 1, 0x00, 0);
	m += put_tlv(inner + m, 0x02, 2, 0x7f, 0);
	m += put_tlv(inner + m, 0x02, 9, 0xff, 0);
	m += put_tlv(inner + m, 0xA2, 40, 0x04, 0);
	m += put_tlv(inner + m, 0xA3, 200, 0x04, 1);
	m += put_tlv(inner + m, 0x85, 30, 0x04, 0);
	n = put_tlv(seed, 0x30, m, 0, 2);
	memcpy(seed + n - m, inner, m);

	for (unsigned long k = 0; k < 2000000; k++) {
		uint8_t tmp[600];
		uint32_t len = n;

		memcpy(tmp, seed, n);
		for (int j = (int)(next() % 6); j >= 0; j--) {
			uint64_t r = next();
			switch (r % 4) {
			case 0: tmp[(r >> 8) % len] = (uint8_t)(r >> 20); break;
			case 1: tmp[(r >> 8) % len] ^= (uint8_t)(1u << ((r >> 20) % 8)); break;
			case 2: tmp[(r >> 8) % 8] = (uint8_t)(r >> 20); break;                  /* the outer header */
			default: len = 1 + (uint32_t)((r >> 8) % len); break;                  /* cut short */
			}
		}
		uint8_t *b = exact(tmp, len);
		walk(b, len);
		free(b);
	}
}

int
main(void)
{
	test_good();
	test_refused();
	test_integers();
	test_mutated();
	printf("exynos_tee_der_test: %lu checks, %lu failed\n", checks, failures);
	return failures ? 1 : 0;
}
