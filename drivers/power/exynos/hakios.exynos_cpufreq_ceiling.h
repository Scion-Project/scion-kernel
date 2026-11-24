/* SPDX-License-Identifier: MIT-0 */
/*
 * The one rule for the Exynos 9810's M3s (the big cores) above 1794 MHz, shared by every path of the governor
 * (hakios.exynos_cpufreq.c, iOS 16.5, and the fork's copy, iOS 27) and tested on the host (tools/exynos-cpufreq-ceiling-test.sh).
 * Plain C, no kernel.
 *
 * Samsung's kernel lets the M3s past 1794000 kHz (max-freq-wo-ocp) only while its over-current protection runs and with
 * at most two of them online (hotplug_governor dual_freq 2314000); docs/ios27/OCP.md has the facts. Here the only way
 * past 1794000 is the over-current guard's stage two, ocp_guard=4, and only while all of these hold: stage one armed and
 * alive, a fresh temperature of the big zone under its cooling point (83 C), and no more M3s on than two. Every other
 * option that would take the M3s higher (gov_policy=3's 2314000 with two parked, cpufreq_gov_big_max=) is held at
 * 1794000 by this rule. 16.5's cpufreq_ocp=1 (Anan Jaser's, the PMIC's warning written) does not go through it.
 * NOT run on a phone.
 *
 * The second way (iOS 27, m3off=2; docs/ios27/BIG-CORES.md): Samsung's 9810 kernel never runs its OCP driver (OCP.md, 18
 * November), so what stands between it and 2314000 is the PMIC's CPU over-current warning (OCP_WARN1 bit 7, which S-Boot
 * leaves on and Samsung's kernel writes on again) and the hotplug governor's count of M3s online. So with m3off=2: the
 * PMIC's warning read on (ocp_guard=1 or more reads it; never written here), a fresh big-zone temperature under 75 C (this
 * path's own stop, under Samsung's 83 C cooling point because it raises the power), and at most two M3s on, "on" meaning
 * not taken offline by CPU_OFF and read off by the PMU. Then 2314000 at most, as Samsung's dual_freq.
 */
#ifndef HAKIOS_EXYNOS_CPUFREQ_CEILING_H
#define HAKIOS_EXYNOS_CPUFREQ_CEILING_H

#include <stdbool.h>
#include <stdint.h>

#define CEIL_HELD_KHZ   1794000u        /* max-freq-wo-ocp: the M3s' ceiling unless the rule allows more */
#define CEIL_DUAL_KHZ   2314000u        /* hotplug_governor single_freq, dual_freq: the most it ever allows */
#define CEIL_DUAL_CORES 2u              /* ... with this many M3s on at most */
#define CEIL_M3S        4u              /* the Exynos 9810's M3s */
#define CEIL_COOL_MC    83000           /* BIG's control point */
#define CEIL_NOT_READ_MC (-273000)      /* the TMU driver's "no reading" */
#define CEIL_M3OFF_STOP_MC 75000        /* m3off=2: its stop, under BIG's control point */

/* what the rule looks at, as the governor knows it now */
struct ceil_in {
	uint32_t guard;         /* ocp_guard= (0: off) */
	bool alive;             /* stage one armed in this boot, its thread running, nothing latched dead */
	bool temp_fresh;        /* the big zone's temperature read and recent */
	int32_t big_mc;         /* that temperature, millidegrees (-273000 or under: not read) */
	uint32_t m3s_off;       /* of the four M3s, those taken offline by CPU_OFF (and read off by the PMU) */
	uint32_t m3off;         /* m3off= (2: the M3 offline path, the second way) */
	bool pmic_on;           /* the PMIC's CPU over-current warning read on (OCP_WARN1 bit 7) */
};

/* why the M3s are held at 1794000 (CEIL_ALLOWED: they are not; 0 is none, for a caller's "not said yet") */
enum ceil_why {
	CEIL_ALLOWED = 1, CEIL_LEVEL, CEIL_NOT_ALIVE, CEIL_NO_TEMP, CEIL_HOT, CEIL_M3S_ON, CEIL_NO_WARN,
};

/* the M3s on: online in Samsung's sense (an M3 parked in WFI or powered down in idle is on) */
static inline uint32_t
ceil_m3s_on(const struct ceil_in *in)
{
	return CEIL_M3S - (in->m3s_off < CEIL_M3S ? in->m3s_off : CEIL_M3S);
}

static inline enum ceil_why
ceil_why(const struct ceil_in *in)
{
	if (in->m3off >= 2) {
		/* the second way: Samsung's own conditions, the PMIC warning and the count; a lower temperature stop */
		if (!in->pmic_on) {
			return CEIL_NO_WARN;
		}
		if (!in->temp_fresh || in->big_mc <= CEIL_NOT_READ_MC) {
			return CEIL_NO_TEMP;
		}
		if (in->big_mc >= CEIL_M3OFF_STOP_MC) {
			return CEIL_HOT;
		}
		return ceil_m3s_on(in) > CEIL_DUAL_CORES ? CEIL_M3S_ON : CEIL_ALLOWED;
	}
	if (in->guard < 4) {
		return CEIL_LEVEL;
	}
	if (!in->alive) {
		return CEIL_NOT_ALIVE;
	}
	if (!in->temp_fresh || in->big_mc <= CEIL_NOT_READ_MC) {
		return CEIL_NO_TEMP;
	}
	if (in->big_mc >= CEIL_COOL_MC) {
		return CEIL_HOT;
	}
	return ceil_m3s_on(in) > CEIL_DUAL_CORES ? CEIL_M3S_ON : CEIL_ALLOWED;
}

static inline const char *
ceil_reason(enum ceil_why why)
{
	switch (why) {
	case CEIL_ALLOWED:   return "ALLOWED: 2314000";
	case CEIL_LEVEL:     return "level under 4";
	case CEIL_NOT_ALIVE: return "stage one not armed or not alive";
	case CEIL_NO_TEMP:   return "no fresh temperature";
	case CEIL_HOT:       return "the big zone at its cooling point";
	case CEIL_M3S_ON:    return "more M3s on than Samsung allows above 1794000 (two at most)";
	case CEIL_NO_WARN:   return "the PMIC's CPU over-current warning not read on (needs ocp_guard=1)";
	}
	return "?";
}

/* the M3s' ceiling: max as the options worked it out, held at 1794000 unless the rule allows more, 2314000 at most */
static inline uint32_t
ceil_big(uint32_t max, enum ceil_why why)
{
	uint32_t top = why == CEIL_ALLOWED ? CEIL_DUAL_KHZ : CEIL_HELD_KHZ;

	return max < top ? max : top;
}

#endif /* HAKIOS_EXYNOS_CPUFREQ_CEILING_H */
