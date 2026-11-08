#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef unsigned __int128 uint128_t;
typedef struct { union { uint128_t q[32]; } v; uint32_t fpsr, fpcr; } arm_neon_saved_state64_t;
struct uneon { arm_neon_saved_state64_t ns_64; };
struct thread { struct { struct uneon *uNeon; } machine; };
typedef struct thread *thread_t;
static struct uneon UN; static struct thread T = { { &UN } };
static thread_t current_thread(void) { return &T; }
#include "sha_part.c"   /* run.sh: the FEAT_SHA3 section of osfmk/arm64/sweet_a64e.c */
struct io { uint8_t v[4][16]; uint32_t word, pad[3]; };
#define T_(name, ins) static void name(struct io *io) { __asm__ volatile( \
  "ldr q0, [%0]\n ldr q1, [%0, #16]\n ldr q2, [%0, #32]\n ldr q3, [%0, #48]\n" \
  "1: " ins "\n str q0, [%0]\n adr x10, 1b\n ldr w10, [x10]\n str w10, [%0, #64]\n" \
  :: "r"(io) : "x10", "v0", "v1", "v2", "v3", "memory"); }
T_(t_eor3, "eor3 v0.16b, v1.16b, v2.16b, v3.16b")
T_(t_bcax, "bcax v0.16b, v1.16b, v2.16b, v3.16b")
T_(t_rax1, "rax1 v0.2d, v1.2d, v2.2d")
T_(t_xar0, "xar v0.2d, v1.2d, v2.2d, #0")
T_(t_xar1, "xar v0.2d, v1.2d, v2.2d, #1")
T_(t_xar37, "xar v0.2d, v1.2d, v2.2d, #37")
T_(t_xar63, "xar v0.2d, v1.2d, v2.2d, #63")
T_(t_h, "sha512h q0, q1, v2.2d")
T_(t_h2, "sha512h2 q0, q1, v2.2d")
T_(t_su0, "sha512su0 v0.2d, v1.2d")
T_(t_su1, "sha512su1 v0.2d, v1.2d, v2.2d")
static void (*tests[])(struct io *) = { t_eor3, t_bcax, t_rax1, t_xar0, t_xar1, t_xar37, t_xar63, t_h, t_h2, t_su0, t_su1 };
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
int main(void) {
	long bad = 0, total = 0;
	for (unsigned t = 0; t < sizeof(tests)/sizeof(tests[0]); t++)
		for (int i = 0; i < 200000; i++) {
			struct io io; uint64_t in[8];
			for (int k = 0; k < 8; k++) in[k] = rnd();
			memcpy(io.v, in, 64);
			tests[t](&io);
			for (int k = 0; k < 4; k++) memcpy(&UN.ns_64.v.q[k], (uint8_t *)in + 16 * k, 16);
			bool ok = emulate_sha3_sha512(io.word);
			total++;
			if (!ok || memcmp(&UN.ns_64.v.q[0], io.v[0], 16)) { if (bad++ < 5) printf("MISMATCH %08x ok %d\n", io.word, ok); }
		}
	printf("%ld cases, %ld mismatches\n", total, bad);
	return bad != 0;
}
