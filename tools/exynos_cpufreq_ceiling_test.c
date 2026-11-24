/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/exynos_cpufreq_ceiling_test.c: drivers/power/exynos/hakios.exynos_cpufreq_ceiling.h on a computer, under the
 * address and undefined-behaviour sanitizers (tools/exynos-cpufreq-ceiling-test.sh). The rule every option of the governor
 * goes through before the Galaxy S9's M3s may run above 1794000 kHz (docs/ios27/OCP.md).
 *
 * What is checked:
 *   - each reason, in the order the rule looks: the level under 4, stage one not alive, no fresh temperature (or the TMU
 *     driver's "not read"), the big zone at 83 C or more, more than two M3s on; and ALLOWED only with all of them clear;
 *   - the edges: ocp_guard 3 and 4, 82999 and 83000 mC, two and three M3s on;
 *   - the ceiling: any rate at or under 1794000 left as it is whatever the reason; above it held at 1794000 unless allowed,
 *     and then 2314000 at most (2704000 and 2886000 never);
 *   - every combination of the inputs over a grid: the ceiling is never above 1794000 unless every condition holds.
 *   - the second way (m3off=2): the PMIC warning, a fresh temperature under 75 C and at most two M3s on, each alone
 *     holding at 1794000; ocp_guard's level and stage one do not matter there; the same grid.
 */
#include <stdio.h>

#include "../drivers/power/exynos/hakios.exynos_cpufreq_ceiling.h"

static unsigned long checks, failures;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

static const struct ceil_in good = { .guard = 4, .alive = true, .temp_fresh = true, .big_mc = 60000, .m3s_off = 2 };

static enum ceil_why
why_with(void (*change)(struct ceil_in *))
{
	struct ceil_in in = good;

	change(&in);
	return ceil_why(&in);
}

static void guard_off(struct ceil_in *in) { in->guard = 0; }
static void guard_3(struct ceil_in *in) { in->guard = 3; }
static void not_alive(struct ceil_in *in) { in->alive = false; }
static void stale(struct ceil_in *in) { in->temp_fresh = false; }
static void not_read(struct ceil_in *in) { in->big_mc = CEIL_NOT_READ_MC; }
static void hot(struct ceil_in *in) { in->big_mc = 83000; }
static void just_cool(struct ceil_in *in) { in->big_mc = 82999; }
static void three_on(struct ceil_in *in) { in->m3s_off = 1; }
static void four_on(struct ceil_in *in) { in->m3s_off = 0; }
static void one_on(struct ceil_in *in) { in->m3s_off = 3; }
static void all_bad(struct ceil_in *in) { in->guard = 1; in->alive = false; in->temp_fresh = false; in->m3s_off = 0; }
static void none(struct ceil_in *in) { (void)in; }

static const struct ceil_in good2 = { .guard = 1, .temp_fresh = true, .big_mc = 60000, .m3s_off = 2, .m3off = 2, .pmic_on = true };

static enum ceil_why
why2_with(void (*change)(struct ceil_in *))
{
	struct ceil_in in = good2;

	change(&in);
	return ceil_why(&in);
}

static void no_warn(struct ceil_in *in) { in->pmic_on = false; }
static void stop_75(struct ceil_in *in) { in->big_mc = 75000; }
static void under_75(struct ceil_in *in) { in->big_mc = 74999; }
static void guard_0(struct ceil_in *in) { in->guard = 0; in->alive = false; in->pmic_on = false; }
static void m3off_1(struct ceil_in *in) { in->m3off = 1; }

int
main(void)
{
	static const uint32_t rates[] = { 650000, 1261000, 1469000, 1794000, 1924000, 2106000, 2314000, 2704000, 2886000 };
	static const uint32_t guards[] = { 0, 1, 2, 3, 4 };
	static const int32_t temps[] = { CEIL_NOT_READ_MC, -40000, 0, 45000, 82999, 83000, 96000 };

	CHECK(why_with(none) == CEIL_ALLOWED, "all clear");
	CHECK(why_with(guard_off) == CEIL_LEVEL, "ocp_guard off");
	CHECK(why_with(guard_3) == CEIL_LEVEL, "ocp_guard=3");
	CHECK(why_with(not_alive) == CEIL_NOT_ALIVE, "stage one not alive");
	CHECK(why_with(stale) == CEIL_NO_TEMP, "no fresh temperature");
	CHECK(why_with(not_read) == CEIL_NO_TEMP, "not read");
	CHECK(why_with(hot) == CEIL_HOT, "83 C");
	CHECK(why_with(just_cool) == CEIL_ALLOWED, "82.999 C");
	CHECK(why_with(three_on) == CEIL_M3S_ON, "three M3s on");
	CHECK(why_with(four_on) == CEIL_M3S_ON, "four M3s on");
	CHECK(why_with(one_on) == CEIL_ALLOWED, "one M3 on");
	CHECK(why_with(all_bad) == CEIL_LEVEL, "the level is the first reason");
	CHECK(ceil_why(&(struct ceil_in){ .guard = 0 }) == CEIL_LEVEL, "nothing known: held");
	CHECK(ceil_m3s_on(&(struct ceil_in){ .m3s_off = 9 }) == 0, "more off than there are: none on");

	CHECK(why2_with(none) == CEIL_ALLOWED, "m3off=2: all clear");
	CHECK(why2_with(no_warn) == CEIL_NO_WARN, "m3off=2: no PMIC warning");
	CHECK(why2_with(stale) == CEIL_NO_TEMP, "m3off=2: no fresh temperature");
	CHECK(why2_with(not_read) == CEIL_NO_TEMP, "m3off=2: not read");
	CHECK(why2_with(stop_75) == CEIL_HOT, "m3off=2: 75 C");
	CHECK(why2_with(under_75) == CEIL_ALLOWED, "m3off=2: 74.999 C");
	CHECK(why2_with(three_on) == CEIL_M3S_ON, "m3off=2: three M3s on");
	CHECK(why2_with(four_on) == CEIL_M3S_ON, "m3off=2: four M3s on");
	CHECK(why2_with(one_on) == CEIL_ALLOWED, "m3off=2: one M3 on");
	CHECK(why2_with(guard_0) == CEIL_NO_WARN, "m3off=2 without ocp_guard: no warning read");
	CHECK(why2_with(m3off_1) == CEIL_LEVEL, "m3off=1 is not the second way");

	for (enum ceil_why w = CEIL_ALLOWED; w <= CEIL_NO_WARN; w++) {
		CHECK(ceil_reason(w)[0] != '\0' && ceil_reason(w)[0] != '?', "reason %d", (int)w);
		for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
			uint32_t got = ceil_big(rates[r], w);

			if (rates[r] <= CEIL_HELD_KHZ) {
				CHECK(got == rates[r], "%u at reason %d: %u, should be left", rates[r], (int)w, got);
			} else if (w == CEIL_ALLOWED) {
				CHECK(got == (rates[r] < CEIL_DUAL_KHZ ? rates[r] : CEIL_DUAL_KHZ), "%u allowed: %u", rates[r], got);
			} else {
				CHECK(got == CEIL_HELD_KHZ, "%u at reason %d: %u, should be held at 1794000", rates[r], (int)w, got);
			}
		}
	}

	for (unsigned g = 0; g < sizeof(guards) / sizeof(guards[0]); g++) {
		for (int a = 0; a < 2; a++) {
			for (int f = 0; f < 2; f++) {
				for (unsigned t = 0; t < sizeof(temps) / sizeof(temps[0]); t++) {
					for (uint32_t off = 0; off <= 5; off++) {
						struct ceil_in in = { guards[g], a != 0, f != 0, temps[t], off, 0, false };
						uint32_t on = off < 4 ? 4 - off : 0;
						bool all = guards[g] >= 4 && a && f && temps[t] > CEIL_NOT_READ_MC && temps[t] < CEIL_COOL_MC &&
						    on <= CEIL_DUAL_CORES;
						uint32_t top = ceil_big(2886000, ceil_why(&in));

						CHECK(all ? top == CEIL_DUAL_KHZ : top == CEIL_HELD_KHZ, "guard %u alive %d fresh %d %d mC %u on: %u",
						    guards[g], a, f, temps[t], on, top);
					}
				}
			}
		}
	}

	for (int w = 0; w < 2; w++) {
		for (int f = 0; f < 2; f++) {
			for (unsigned t = 0; t < sizeof(temps) / sizeof(temps[0]); t++) {
				for (uint32_t off = 0; off <= 5; off++) {
					static const int32_t more[] = { 74999, 75000 };
					for (unsigned x = 0; x < 3; x++) {
						int32_t mc = x < 2 ? more[x] : temps[t];
						struct ceil_in in = { .guard = (uint32_t)w, .temp_fresh = f != 0, .big_mc = mc, .m3s_off = off,
							              .m3off = 2, .pmic_on = w != 0 };
						uint32_t on = off < 4 ? 4 - off : 0;
						bool all = w && f && mc > CEIL_NOT_READ_MC && mc < CEIL_M3OFF_STOP_MC && on <= CEIL_DUAL_CORES;
						uint32_t top = ceil_big(2886000, ceil_why(&in));

						CHECK(all ? top == CEIL_DUAL_KHZ : top == CEIL_HELD_KHZ, "m3off=2 warn %d fresh %d %d mC %u on: %u",
						    w, f, mc, on, top);
					}
				}
			}
		}
	}

	printf("exynos_cpufreq_ceiling_test: %lu checks, %lu failures\n", checks, failures);
	return failures != 0;
}
