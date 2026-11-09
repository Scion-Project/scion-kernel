// LSE rewrite test driver: deterministic per-case trials printed for diffing (original vs rewritten
// binary), then multithreaded atomicity stress.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "cases.h"

extern void st_ldaddal(volatile uint64_t *, uint64_t);
extern void st_casal_inc(volatile uint64_t *, uint64_t);
extern void st_caspal_inc(volatile uint64_t *, uint64_t);
extern void st_swpal_lock(volatile uint64_t *, uint64_t);
extern void st_ldsmaxal(volatile uint64_t *, uint64_t, uint64_t);

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static uint8_t stackbuf[65536] __attribute__((aligned(16)));
enum { NT = 8, N = 2000000 };
static volatile uint64_t gcnt[4] __attribute__((aligned(64)));
static uint8_t membuf[256] __attribute__((aligned(64)));

void *f1(void *a) { st_ldaddal(&gcnt[0], N); return a; }
void *f2(void *a) { st_casal_inc(&gcnt[0], N); return a; }
void *f3(void *a) { st_caspal_inc(&gcnt[0], N / 4); return a; }
void *f4(void *a) { st_swpal_lock(&gcnt[0], N / 4); return a; }
void *f5(void *a) { st_ldsmaxal(&gcnt[0], N, (uint64_t)(long)a); return a; }
int main(int argc, char **argv) {
    int stress = argc > 1 ? atoi(argv[1]) : 1;
    unsigned long st[34];
    setvbuf(stdout, 0, _IOLBF, 0);
    for (int i = 0; i < NCASES; i++) {
        for (int trial = 0; trial < 6; trial++) {
            for (int r = 0; r < 31; r++) st[r] = rnd();
            st[18] = 0;
            if (trial & 1) for (int r = 0; r < 31; r++) st[r] &= 0xff;           // small values (signedness)
            uint8_t *mem;
            st[31] = 0;
            if (T[i].rn == 31) {
                mem = stackbuf + 32768;
                st[31] = (unsigned long)mem;
            } else {
                mem = membuf + 64;
                st[T[i].rn] = (unsigned long)mem;
            }
            st[32] = (rnd() & 0xf) << 28;                                        // NZCV
            for (int k = -64; k < 64; k++) mem[k] = (uint8_t)rnd();
            unsigned long rsv = T[i].rs == 31 ? 0 : st[T[i].rs];
            if (trial >= 2 && trial < 4) {           // make compares succeed
                if (T[i].kind == 0) memcpy(mem, &rsv, 1u << T[i].size);
                if (T[i].kind == 1) {
                    unsigned b = 1u << T[i].size;
                    memcpy(mem, &st[T[i].rs], b); memcpy(mem + b, &st[T[i].rs + 1], b);
                }
            }
            if (trial == 4 && T[i].kind == 0) {      // equal low bits, garbage high bits in Rs
                memcpy(mem, &rsv, 1u << T[i].size);
                if (T[i].rs != 31 && T[i].rs != T[i].rn && T[i].size < 3) st[T[i].rs] ^= 0xdead000000000000ull;
            }
            uint8_t before[128];
            memcpy(before, mem - 64, 128);
            T[i].fn(st);
            if (T[i].rn != 31) st[31] = 0; else st[31] -= (unsigned long)mem;   // must come back 0
            // pointer registers differ between runs only through mem addresses: normalise
            for (int r = 0; r < 31; r++)
                if (st[r] == (unsigned long)mem) st[r] = 0x5151;
            printf("%d.%d %s |", i, trial, T[i].name);
            for (int r = 0; r < 33; r++) printf(" %lx", st[r]);
            printf(" | m");
            for (int k = -64; k < 64; k++) if (mem[k] != before[k + 64]) printf(" %d:%02x", k, mem[k]);
            printf("\n");
        }
    }
    if (!stress) return 0;
    // stress: 8 threads
    volatile uint64_t *cnt = gcnt;
    pthread_t th[NT];
#define RUN(fnbody) do { for (int t = 0; t < NT; t++) pthread_create(&th[t], 0, fnbody, (void *)(long)t); \
                         for (int t = 0; t < NT; t++) pthread_join(th[t], 0); } while (0)
    cnt[0] = 0; RUN(f1); printf("STRESS ldaddal %llu expect %llu %s\n", cnt[0], (unsigned long long)NT * N, cnt[0] == (uint64_t)NT * N ? "OK" : "FAIL");
    cnt[0] = 0; RUN(f2); printf("STRESS casal %llu expect %llu %s\n", cnt[0], (unsigned long long)NT * N, cnt[0] == (uint64_t)NT * N ? "OK" : "FAIL");
    cnt[0] = 0; cnt[1] = ~0ull; RUN(f3); printf("STRESS caspal %llu/%llx expect %llu %s\n", cnt[0], cnt[1], (unsigned long long)NT * (N / 4), (cnt[0] == (uint64_t)NT * (N / 4) && cnt[1] == ~cnt[0]) ? "OK" : "FAIL");
    cnt[0] = 0; cnt[1] = 0; RUN(f4); printf("STRESS swpal-lock %llu expect %llu %s\n", cnt[1], (unsigned long long)NT * (N / 4), cnt[1] == (uint64_t)NT * (N / 4) ? "OK" : "FAIL");
    cnt[0] = 0; RUN(f5); { uint64_t mx = 0; for (int t = 0; t < NT; t++) { uint64_t v = (uint64_t)t + 4ull * (N - 1); if (v > mx) mx = v; }
        printf("STRESS ldsmaxal %llu expect %llu %s\n", cnt[0], mx, cnt[0] == mx ? "OK" : "FAIL"); }
    return 0;
}
