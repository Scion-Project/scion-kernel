#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned __int128 uint128_t;
typedef struct { union { uint128_t q[32]; } v; uint32_t fpsr, fpcr; } arm_neon_saved_state64_t;
struct uneon { arm_neon_saved_state64_t ns_64; };
struct thread { struct { struct uneon *uNeon; } machine; };
typedef struct thread *thread_t;
static struct uneon UN; static struct thread T = { { &UN } };
static thread_t current_thread(void) { return &T; }
#include "fhm_part.c"   /* run.sh: the FEAT_FHM section of osfmk/arm64/sweet_a64e.c */

struct io { uint8_t v0[16], v1[16], v2[16]; uint32_t fpcr, fpsr, word, pad; };
#define T_(name, ins) static void name(struct io *io) { __asm__ volatile( \
  "ldr q0, [%0]\n ldr q1, [%0, #16]\n ldr q2, [%0, #32]\n ldr w9, [%0, #48]\n msr fpcr, x9\n msr fpsr, xzr\n" \
  "1: " ins "\n" \
  "str q0, [%0]\n mrs x9, fpsr\n str w9, [%0, #52]\n msr fpcr, xzr\n adr x10, 1b\n ldr w10, [x10]\n str w10, [%0, #56]\n" \
  :: "r"(io) : "x9", "x10", "v0", "v1", "v2", "memory"); }
#define E8(op, q, t) T_(op##q##_0, #op " v0." #q ", v1." #t ", v2.h[0]") T_(op##q##_1, #op " v0." #q ", v1." #t ", v2.h[1]") \
 T_(op##q##_2, #op " v0." #q ", v1." #t ", v2.h[2]") T_(op##q##_3, #op " v0." #q ", v1." #t ", v2.h[3]") \
 T_(op##q##_4, #op " v0." #q ", v1." #t ", v2.h[4]") T_(op##q##_5, #op " v0." #q ", v1." #t ", v2.h[5]") \
 T_(op##q##_6, #op " v0." #q ", v1." #t ", v2.h[6]") T_(op##q##_7, #op " v0." #q ", v1." #t ", v2.h[7]")
#define V(op, q, t) T_(op##q##_v, #op " v0." #q ", v1." #t ", v2." #t)
#define ALL(op) V(op, 4s, 4h) V(op, 2s, 2h) E8(op, 4s, 4h) E8(op, 2s, 2h)
ALL(fmlal) ALL(fmlal2) ALL(fmlsl) ALL(fmlsl2)
#define L8(op, q) op##q##_0, op##q##_1, op##q##_2, op##q##_3, op##q##_4, op##q##_5, op##q##_6, op##q##_7
#define LALL(op) op##4s_v, op##2s_v, L8(op, 4s), L8(op, 2s)
static void (*tests[])(struct io *) = { LALL(fmlal), LALL(fmlal2), LALL(fmlsl), LALL(fmlsl2) };

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint16_t rh(void) {
	uint16_t s = (rnd() & 1) << 15;
	switch (rnd() % 12) {
	case 0: return s;
	case 1: return s | 1;
	case 2: return s | (rnd() & 0x3ff);                      /* subnormal */
	case 3: return s | 0x7c00;                               /* inf */
	case 4: return s | 0x7e00 | (rnd() & 0x1ff);             /* qNaN */
	case 5: return s | 0x7c00 | ((rnd() & 0x1ff) | 1);       /* sNaN */
	case 6: return s | 0x7bff;
	case 7: return s | 0x3c00;
	default: return s | (rnd() % 0x7c00);
	}
}
static uint32_t rsgl(uint16_t n, uint16_t m) {
	uint32_t s = (rnd() & 1) << 31;
	switch (rnd() % 14) {
	case 0: return s;
	case 1: return s | (rnd() & 0x7fffff);                   /* subnormal */
	case 2: return s | 0x7f800000;
	case 3: return s | 0x7fc00000 | (rnd() & 0x3fffff);
	case 4: return s | 0x7f800000 | ((rnd() & 0x3fffff) | 1);
	case 5: return s | 0x7f7fffff;
	case 6: case 7: case 8: {                               /* near -(n*m): cancellation, rounding */
		float fn, fm, p; uint32_t b;
		uint32_t bn = h_to_s(n), bm = h_to_s(m);
		memcpy(&fn, &bn, 4); memcpy(&fm, &bm, 4); p = -(fn * fm);
		memcpy(&b, &p, 4);
		return b + (uint32_t)((int)(rnd() % 64) - 32);
	}
	case 9: return s | (1u << 23) | (rnd() & 0xffff);        /* tiny normal */
	default: return rnd();
	}
}
int main(int argc, char **argv) {
	long iters = argc > 1 ? atol(argv[1]) : 20000, bad = 0, total = 0;
	static const uint32_t fpcrs[] = { 0, 1u<<22, 2u<<22, 3u<<22, 1u<<24, 1u<<25, 1u<<19, 1u<<26,
	    (1u<<24)|(1u<<19), (1u<<25)|(1u<<19)|(3u<<22), (1u<<24)|(1u<<25)|(1u<<19)|(1u<<22)|(1u<<26) };
	for (unsigned t = 0; t < sizeof(tests)/sizeof(tests[0]); t++) {
		for (long i = 0; i < iters; i++) {
			struct io io; uint16_t n[8], m[8]; uint32_t d[4];
			for (int k = 0; k < 8; k++) { n[k] = rh(); m[k] = rh(); }
			for (int k = 0; k < 4; k++) d[k] = rsgl(n[k], m[k ^ (rnd() & 7)]);
			memcpy(io.v0, d, 16); memcpy(io.v1, n, 16); memcpy(io.v2, m, 16);
			io.fpcr = fpcrs[rnd() % (sizeof(fpcrs)/sizeof(fpcrs[0]))];
			tests[t](&io);
			memcpy(&UN.ns_64.v.q[0], d, 16); memcpy(&UN.ns_64.v.q[1], n, 16); memcpy(&UN.ns_64.v.q[2], m, 16);
			UN.ns_64.fpcr = io.fpcr; UN.ns_64.fpsr = 0;
			bool ok = emulate_fmlal(io.word);
			uint32_t flags = io.fpsr & 0x9f;
			total++;
			if (!ok || memcmp(&UN.ns_64.v.q[0], io.v0, 16) || (UN.ns_64.fpsr & 0x9f) != flags) {
				if (bad++ < 10) {
					uint32_t e[4], h[4]; memcpy(e, &UN.ns_64.v.q[0], 16); memcpy(h, io.v0, 16);
					printf("MISMATCH %08x fpcr %08x ok %d hw fpsr %02x emu %02x\n  d %08x %08x %08x %08x\n  n %04x %04x %04x %04x %04x %04x %04x %04x\n  m %04x %04x %04x %04x %04x %04x %04x %04x\n  hw  %08x %08x %08x %08x\n  emu %08x %08x %08x %08x\n",
					    io.word, io.fpcr, ok, flags, UN.ns_64.fpsr, d[0], d[1], d[2], d[3], n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7],
					    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], h[0], h[1], h[2], h[3], e[0], e[1], e[2], e[3]);
				}
			}
		}
	}
	printf("%ld cases, %ld mismatches, %zu encodings\n", total, bad, sizeof(tests)/sizeof(tests[0]));
	return bad != 0;
}
