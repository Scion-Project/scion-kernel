/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's second entry (base + 0x40, xnu_boot.S warm_entry): a running kernel starts this loader again without a
 * reset of the SoC (docs/ios27/WARM-RESTART.md). S-Boot is not in the path: no motor, no Samsung logo, no ten seconds.
 * NOT RUN ON A PHONE YET.
 *
 * The kernel has read BOOT (read only) and put its kernel slot -- this Image and its appended SWEETXNU payload -- where
 * S-Boot puts it, and left a struct scion_reentry (scion_reentry.h) for this code. Here: the record copied and checked
 * (magic, version, CRC-32), the SHA-256 of what is appended to the Image (BOOT's payload) checked against the kernel's (a copy that went wrong, or memory a
 * device wrote into, stops here), then the start as on a cold boot (xnu_boot.c star_warm_start: BOOT's payload through
 * xnu_enter, or an Android kernel through the fastboot path's ram_start). No Bixby window, no splash, no POST, no
 * menu, no storage: nothing is read or written but RAM and the display.
 *
 * The watchdog: the kernel arms the cluster-1 watchdog before it lets go (SCNR_F_WDT_ARMED), so a hang from there to
 * here, or here, resets the SoC (S-Boot, the motor, BOOT again: an ordinary restart). It is stopped right before the
 * OS starts, unless SCNR_F_WDT_KEEP. A record or image that is not right: the reason on the screen, 10 s, then the
 * PMU's restart into recovery (the same as the menu's "reboot to recovery").
 */
#include <stdint.h>
#include "fbtext.h"
#include "hw.h"
#include "mmu.h"
#include "scion_reentry.h"

#define LOG_BASE       0x91fff000UL
#define WDT1_BASE      0x10060000UL     /* cluster 1's watchdog (xnuloader/wdt_exynos.c's facts) */
#define WDT_PMU_BIT    (1U << 23)
#define PMU_WDT_DISABLE 0x408U
#define PMU_WDT_MASK    0x40CU

uintptr_t star_warm_start(volatile uint64_t *log, const struct scion_reentry *r);   /* xnu_boot.c */

static struct scion_reentry warm_rec;

static void
warm_wdt_stop(void)
{
	volatile uint32_t *wdt = (volatile uint32_t *)WDT1_BASE;
	volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;

	wdt[0] = 0;                                     /* WTCON: off */
	pmu[PMU_WDT_MASK / 4] |= WDT_PMU_BIT;           /* its reset request masked again, as S-Boot leaves it */
	__asm__ volatile("dsb sy" : : : "memory");
}

static void
warm_give_up(const char *why)
{
	uint64_t freq, t0, t;
	volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;

	star_fb_text(3, "WARM RESTART FAILED:");
	star_fb_text(4, why);
	star_fb_text(5, "RECOVERY IN 10 S");
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));
	if (!freq) {
		freq = 26000000U;
	}
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (t0));
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (t));
	} while ((t - t0) / freq < 10);
	pmu[0x808 / 4] = 0x12345678U;                   /* INFORM2 */
	pmu[0x80c / 4] = INFORM_RECOVERY;               /* INFORM3 */
	__asm__ volatile("dsb sy" : : : "memory");
	pmu[0x400 / 4] = 1;                             /* SWRESET */
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/*
 * From xnu_boot.S warm_entry: x0 = the record, at EL1 with the MMU off. Returns the OS's entry (log[17] is its x0) or
 * does not return.
 */
uintptr_t star_warm(uintptr_t record, volatile uint64_t *log, uint64_t el, uint64_t sctlr);
uintptr_t
star_warm(uintptr_t record, volatile uint64_t *log, uint64_t el, uint64_t sctlr)
{
	extern char _start[], _image_end[];
	const volatile uint8_t *s = (const volatile uint8_t *)record;
	uint8_t *d = (uint8_t *)&warm_rec;
	const char *why;
	uintptr_t entry;

	log[9] = 0x5741524d;                            /* "WARM": this boot came through warm_entry */
	log[8] = record;
	if (record & 7) {
		warm_give_up("WARM: RECORD NOT ALIGNED");
	}
	for (unsigned i = 0; i < sizeof(warm_rec); i++) {
		d[i] = s[i];                            /* first thing: nothing else in RAM is trusted */
	}
	if (el == 4 && !(sctlr & 1)) {
		star_mmu_enable();                      /* the hash over ~60 MB, and the copy, at cache speed */
	}
	log[77] = star_fb_enable();
	star_fb_text(0, "PROJECT STARBITE");
	star_fb_text(1, "WARM RESTART");
	if (el != 4) {
		warm_give_up("WARM: NOT EL1");
	}
	why = scnr_check(&warm_rec, (uint64_t)(uintptr_t)_start);
	if (why) {
		warm_give_up(why);
	}
	star_fb_text(2, "CHECKING THE PAYLOAD'S SHA-256");
	if (!scnr_image_ok(&warm_rec, (const void *)_start, (uint64_t)(_image_end - _start))) {
		warm_give_up("WARM: IMAGE SHA-256 MISMATCH");
	}
	entry = star_warm_start(log, &warm_rec);
	if (!entry) {
		warm_give_up("WARM: THE OS COULD NOT BE PLACED");
	}
	if ((warm_rec.flags & SCNR_F_WDT_ARMED) && !(warm_rec.flags & SCNR_F_WDT_KEEP)) {
		warm_wdt_stop();
	}
	return entry;
}
