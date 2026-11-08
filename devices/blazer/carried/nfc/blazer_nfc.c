/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-nfc-r1/receipts/0001-hk-nfc1.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#if defined(BLAZER)
/* SPDX-License-Identifier: MIT
 * XNU's part of blazer_nfc_hw.c: boot args, mappings, one thread that runs once. Off unless blazer_nfc=N (1..4).
 *   blazer_nfc_delay=S  start after S s (30..600, default 60: clear of bring-up; the bus lock orders it against the haptics)
 *   blazer_nfc_wait=S   stage 4 listen time (1..60, default 20)
 *   blazer_nfc_keep=1   leave the chip enabled at the end
 * Polled CPM frames take blazer_cpmq_enter/exit; the whole run holds the cli10 bus lock (haptic taps meanwhile: EBUSY, skipped). */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include "blazer_nfc_hw.h"
#include "blazer_cpmq_xnu.h"
#include "blazer_cli10_bus.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* Every page blazer_nfc_hw.c's pa_allowed() names, mapped once */
static struct {
	uint64_t pa;
	vm_offset_t va;
} windows[] = {
	{ 0x0c581000ULL, 0 }, { 0x0c583000ULL, 0 }, { 0x06483000ULL, 0 },      /* chip enable, power state, host IRQ: stage 1 */
	{ 0x05250000ULL, 0 }, { 0x05272000ULL, 0 }, { 0x3a330000ULL, 0 }, { 0x3a331000ULL, 0 }, { 0x3a210000ULL, 0 }, { 0x3a211000ULL, 0 },
};
static struct nf nfc;
static struct nf_cfg boot_cfg;
static uint32_t boot_delay_s = 60;

static volatile uint32_t *
at(uint64_t pa)
{
	for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
		if (windows[i].va != 0 && pa >= windows[i].pa && pa < windows[i].pa + 0x1000) {
			return (volatile uint32_t *)(windows[i].va + (pa - windows[i].pa));
		}
	}
	return NULL;
}

static int
k_read32(__unused void *ctx, uint64_t pa, uint32_t *value)
{
	volatile uint32_t *p = at(pa);

	if (p == NULL) {
		return -1;
	}
	*value = *p;
	return 0;
}

static int
k_write32(__unused void *ctx, uint64_t pa, uint32_t value)
{
	volatile uint32_t *p = at(pa);

	if (p == NULL) {
		return -1;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*p = value;
	__asm__ volatile ("dsb sy" ::: "memory");
	return 0;
}

static void
k_smc(__unused void *ctx, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t r[2])
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc");
	r[0] = x0;
	r[1] = x1;
}

static void k_delay_us(__unused void *ctx, uint32_t us) { delay_for_interval(us, NSEC_PER_USEC); }

static void
k_log(__unused void *ctx, const char *format, ...)
{
	va_list args;

	printf("blazer-nfc: ");
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

static void k_cpm_enter(__unused void *ctx) { blazer_cpmq_enter(); }
static void k_cpm_exit(__unused void *ctx) { blazer_cpmq_exit(); }
static void k_bus_enter(__unused void *ctx) { blazer_cli10_enter(); }
static void k_bus_exit(__unused void *ctx) { blazer_cli10_exit(); }

static void
nfc_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const struct nf_ops ops = { .read32 = k_read32, .write32 = k_write32, .smc = k_smc, .delay_us = k_delay_us, .log = k_log,
		                           .cpm_enter = k_cpm_enter, .cpm_exit = k_cpm_exit, .bus_enter = k_bus_enter, .bus_exit = k_bus_exit };
	unsigned mapped = boot_cfg.stage >= NF_STAGE_PROBE ? sizeof(windows) / sizeof(windows[0]) : 3;

	printf("blazer-nfc: stage %u, starts in %u s\n", boot_cfg.stage, boot_delay_s);
	delay_for_interval(boot_delay_s * 1000, NSEC_PER_MSEC);
	for (unsigned i = 0; i < mapped; i++) {
		windows[i].va = ml_io_map((vm_offset_t)windows[i].pa, 0x1000);
	}
	nf_init(&nfc, &ops, &boot_cfg);
	(void)nf_run(&nfc);
	printf("blazer-nfc: ends: %s, stage %u, chip %s, fw 0x%08x, ntfs %u, tags %u\n", nf_stop_name(nfc.stop), boot_cfg.stage,
	    nfc.fw_version || nfc.ntf_count ? "answered" : nfc.addr_ack ? "acked" : "silent", nfc.fw_version, nfc.ntf_count, nfc.tags);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void blazer_nfc_init(void);
void
blazer_nfc_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("blazer_nfc", &boot_cfg.stage, sizeof(boot_cfg.stage)) || boot_cfg.stage == 0) {
		return;
	}
	if (boot_cfg.stage > NF_STAGE_DISCOVER) {
		printf("blazer-nfc: stage %u unknown, off\n", boot_cfg.stage);
		return;
	}
	(void)PE_parse_boot_argn("blazer_nfc_wait", &boot_cfg.listen_s, sizeof(boot_cfg.listen_s));
	(void)PE_parse_boot_argn("blazer_nfc_keep", &boot_cfg.keep, sizeof(boot_cfg.keep));
	(void)PE_parse_boot_argn("blazer_nfc_delay", &boot_delay_s, sizeof(boot_delay_s));
	if (boot_delay_s < 30 || boot_delay_s > 600) {
		boot_delay_s = 60;
	}
	if (kernel_thread_start(nfc_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* BLAZER */
