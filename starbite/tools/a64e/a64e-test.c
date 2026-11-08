/*
 * arm64e instruction test for the S9 (no PAC hardware): every instruction
 * sweet_a64e.c emulates, run as a static arm64e binary with no libc, results
 * written to /dev/console ("A64E TEST ..."). Built by build-a64e-test.sh.
 */
#include <stdint.h>

static long
sys3(long n, long a, long b, long c)
{
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register long x16 __asm__("x16") = n;
	__asm__ volatile ("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
	return x0;
}

static int fd = 1;
static int failures;

static void
out(const char *s)
{
	long n = 0;
	while (s[n]) {
		n++;
	}
	sys3(4, fd, (long)s, n);
}

static void
hex(uint64_t v)
{
	char b[19] = "0x";
	for (int i = 0; i < 16; i++) {
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	}
	b[18] = 0;
	out(b);
}

static void
check(const char *name, uint64_t got, uint64_t want)
{
	out("A64E TEST ");
	out(name);
	out(got == want ? " PASS " : " FAIL ");
	hex(got);
	if (got != want) {
		out(" want ");
		hex(want);
		failures++;
	}
	out("\n");
}

__attribute__((noinline)) static uint64_t
leaf_with_frame(uint64_t x)
{
	/* arm64e: pacibsp ... retab around a frame */
	volatile uint64_t keep = x * 3;
	return keep + 1;
}

/* explicit RETAB, and BRAA into it */
__attribute__((naked, noinline)) static uint64_t
ret_auth(uint64_t x)
{
	__asm__ ("pacibsp\n\tadd x0, x0, #100\n\tretab");
}

__attribute__((naked, noinline)) static uint64_t
br_auth(uint64_t x, uint64_t (*fn)(uint64_t))
{
	__asm__ ("mov x9, #77\n\tbraa x1, x9");
}

__attribute__((noinline)) static uint64_t
called_indirect(uint64_t x)
{
	return x + 7;
}

void test_main(void);

/* Static binary entry: the kernel leaves sp at argc (8-byte aligned); align it like crt0. */
__attribute__((naked)) void
_start(void)
{
	__asm__ ("mov x9, sp\n\tand x9, x9, #~15\n\tmov sp, x9\n\tbl _test_main\n\tb .");
}

void
test_main(void)
{
	long f = sys3(5, (long)"/dev/console", 1, 0);
	if (f >= 0) {
		fd = (int)f;
	}
	out("A64E TEST start\n");

	check("frame", leaf_with_frame(5), 16);
	check("retab", ret_auth(1), 101);
	check("braa", br_auth(2, ret_auth), 102);

	/* blraa: arm64e calls through signed function pointers */
	uint64_t (*volatile fp)(uint64_t) = called_indirect;
	check("blraa", fp(1), 8);

	uint64_t p = 0x123456789aULL, mod = 42, r = p;
	__asm__ volatile ("pacia %0, %1" : "+r"(r) : "r"(mod));
	__asm__ volatile ("autia %0, %1" : "+r"(r) : "r"(mod));
	check("pacia+autia", r, p);
	r = p;
	__asm__ volatile ("paciza %0\n\tautiza %0\n\txpaci %0" : "+r"(r));
	check("paciza+autiza+xpaci", r, p);
	r = p;
	__asm__ volatile ("pacda %0, %1\n\tautdb %0, %1\n\txpacd %0" : "+r"(r) : "r"(mod));
	check("pacda+autdb+xpacd", r, p);

	uint64_t g1, g2;
	__asm__ volatile ("pacga %0, %1, %2" : "=r"(g1) : "r"(p), "r"(mod));
	__asm__ volatile ("pacga %0, %1, %2" : "=r"(g2) : "r"(p), "r"(mod));
	check("pacga stable", g1 == g2 && (g1 & 0xffffffff) == 0, 1);

	static uint64_t cell[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
	uint64_t *base = &cell[0], v;
	__asm__ volatile ("ldraa %0, [%1, #16]" : "=r"(v) : "r"(base));
	check("ldraa off", v, 0x3333);
	__asm__ volatile ("ldrab %0, [%1, #8]!" : "=r"(v), "+r"(base));
	check("ldrab pre", v, 0x2222);
	check("ldrab wb", (uint64_t)base, (uint64_t)&cell[1]);

	double d1 = -3.75, d2 = 4294967301.0, d3 = 2147483648.0;
	uint32_t w;
	uint64_t nzcv;
	__asm__ volatile ("fjcvtzs %w0, %d2\n\tmrs %1, nzcv" : "=r"(w), "=r"(nzcv) : "w"(d1));
	check("fjcvtzs -3.75", w | ((nzcv >> 30) & 1) << 32, 0xfffffffdULL);
	__asm__ volatile ("fjcvtzs %w0, %d2\n\tmrs %1, nzcv" : "=r"(w), "=r"(nzcv) : "w"(d2));
	check("fjcvtzs 2^32+5", w | ((nzcv >> 30) & 1) << 32, 5);
	__asm__ volatile ("fjcvtzs %w0, %d2\n\tmrs %1, nzcv" : "=r"(w), "=r"(nzcv) : "w"(d3));
	check("fjcvtzs 2^31", w | ((nzcv >> 30) & 1) << 32, 0x80000000ULL);
	double d4 = -17.0;
	__asm__ volatile ("fjcvtzs %w0, %d2\n\tmrs %1, nzcv" : "=r"(w), "=r"(nzcv) : "w"(d4));
	check("fjcvtzs -17 exact", w | ((nzcv >> 30) & 1) << 32, 0x1ffffffefULL);

	/* fcadd .4s #90: re = n.re - m.im, im = n.im + m.re */
	float n4[4] = { 1, 2, 3, 4 }, m4[4] = { 10, 20, 30, 40 }, d4v[4];
	__asm__ volatile ("ldr q0, [%1]\n\tldr q1, [%2]\n\tfcadd v2.4s, v0.4s, v1.4s, #90\n\tstr q2, [%0]"
	    :: "r"(d4v), "r"(n4), "r"(m4) : "v0", "v1", "v2", "memory");
	union { float f; uint32_t u; } a = { d4v[0] }, b = { d4v[1] }, c = { d4v[2] }, e = { d4v[3] };
	check("fcadd s #90", (uint64_t)a.u << 32 | b.u, (uint64_t)0xc1980000 << 32 | 0x41400000); /* 1-20 = -19, 2+10 = 12 */
	check("fcadd s #90 hi", (uint64_t)c.u << 32 | e.u, (uint64_t)0xc2140000 << 32 | 0x42080000); /* -37, 34 */
	double n2[2] = { 1.5, -2.0 }, m2[2] = { 0.5, 8.0 }, dd[2];
	__asm__ volatile ("ldr q0, [%1]\n\tldr q1, [%2]\n\tfcadd v2.2d, v0.2d, v1.2d, #270\n\tstr q2, [%0]"
	    :: "r"(dd), "r"(n2), "r"(m2) : "v0", "v1", "v2", "memory");
	union { double f; uint64_t u; } re = { dd[0] }, im = { dd[1] };
	check("fcadd d #270 re", re.u, 0x4023000000000000ULL);   /* 1.5 + 8 = 9.5 */
	check("fcadd d #270 im", im.u, 0xc004000000000000ULL);   /* -2 - 0.5 = -2.5 */

	out(failures ? "A64E TEST done: FAILURES\n" : "A64E TEST done: all passed\n");
	sys3(1, failures, 0, 0);
	for (;;) {
	}
}
