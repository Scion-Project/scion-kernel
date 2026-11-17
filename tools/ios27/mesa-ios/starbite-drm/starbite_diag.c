/*
 * Project Starbite: the DRM shim's diagnostics (starbite_drm.c's other half): who holds the GPU's memory (STARBITE_DRM_LEAKS)
 * and the requests counted by kind (STARBITE_DRM_STATS), with the counters Panfrost and PanVK bump for the latter.
 * Split from starbite_drm.c on 17 November 2026 as it was (tools/arch-size.allow); nothing here changes a request.
 *
 * After HakiOS tools/mesa/hakios-drm/hakios.drm.c (MIT, Copyright the HakiOS authors), reworked for Panfrost and StarbiteMali.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "starbite_drm_private.h"

/*
 * Who holds the GPU's memory (the iOS 26 port ran out of RAM with 1.3 GB of buffers wired):
 * every buffer this process creates or imports is kept with the call stack that made it, and
 * every 15 s the call sites holding the most live memory are appended to /var/local/mtl2.log as
 * "SBDRM <proc> live N MiB in M buffers; site: K MiB in J (made T) <- lib+off ...".
 * STARBITE_DRM_LEAKS=0: off. STARBITE_DRM_LEAKS=2: the lines go to the kernel's console as well (StarbiteMali's LOG call,
 * cut to its 250 characters), for a phone whose files cannot be read while it runs.
 */
#define LEAK_FRAMES 10
#define LEAK_SITES  512
#define LEAK_HANDLES 65536
struct leak_site {
	void *pcs[LEAK_FRAMES];
	uint64_t live_bytes, live, made;
};
static struct leak_site leak_sites[LEAK_SITES];
static unsigned leak_nsites;
static struct { uint64_t bytes; uint16_t site; bool used; } leak_handles[LEAK_HANDLES];
static pthread_mutex_t leak_lock = PTHREAD_MUTEX_INITIALIZER;
static int leaks_on = -1;
static time_t leak_last;

static void
leak_dump_locked(void)
{
	FILE *f = fopen("/var/local/mtl2.log", "a");
	if (f == NULL) {
		return;
	}
	uint64_t all = 0, count = 0;
	for (unsigned i = 0; i < leak_nsites; i++) {
		all += leak_sites[i].live_bytes;
		count += leak_sites[i].live;
	}
	fprintf(f, "SBDRM %s[%d] live %llu MiB in %llu buffers\n", getprogname(), getpid(),
	    (unsigned long long)(all >> 20), (unsigned long long)count);
	char con[251];
	if (leaks_on == 2) {
		int n = snprintf(con, sizeof(con), "SBDRM %s[%d] live %llu MiB in %llu buffers", getprogname(), getpid(),
		        (unsigned long long)(all >> 20), (unsigned long long)count);
		sbdrm_kernel_log(con, (size_t)n + 1);
	}
	bool shown[LEAK_SITES] = { false };
	for (unsigned k = 0; k < 8; k++) {
		int best = -1;
		for (unsigned i = 0; i < leak_nsites; i++) {
			if (!shown[i] && leak_sites[i].live_bytes > 0 &&
			    (best < 0 || leak_sites[i].live_bytes > leak_sites[best].live_bytes)) {
				best = (int)i;
			}
		}
		if (best < 0) {
			break;
		}
		shown[best] = true;
		struct leak_site *s = &leak_sites[best];
		fprintf(f, "SBDRM %s  %llu KiB in %llu (made %llu) <-", getprogname(),
		    (unsigned long long)(s->live_bytes >> 10), (unsigned long long)s->live,
		    (unsigned long long)s->made);
		int cn = snprintf(con, sizeof(con), "SBDRM  %llu KiB in %llu (made %llu) <-", (unsigned long long)(s->live_bytes >> 10),
		         (unsigned long long)s->live, (unsigned long long)s->made);
		for (unsigned j = 0; j < LEAK_FRAMES && s->pcs[j] != NULL; j++) {
			Dl_info info;
			if (dladdr(s->pcs[j], &info) && info.dli_fname != NULL) {
				const char *base = strrchr(info.dli_fname, '/');
				fprintf(f, " %s+%#lx", base ? base + 1 : info.dli_fname,
				    (unsigned long)((uintptr_t)s->pcs[j] - (uintptr_t)info.dli_fbase));
				if (cn < (int)sizeof(con) - 1) {
					cn += snprintf(con + cn, sizeof(con) - (size_t)cn, " %.9s+%#lx", base ? base + 1 : info.dli_fname,
					          (unsigned long)((uintptr_t)s->pcs[j] - (uintptr_t)info.dli_fbase));
				}
			} else {
				fprintf(f, " %p", s->pcs[j]);
			}
		}
		fprintf(f, "\n");
		if (leaks_on == 2) {
			con[sizeof(con) - 1] = 0;
			sbdrm_kernel_log(con, strlen(con) + 1);
		}
	}
	fclose(f);
}

void
sbdrm_leak_note(uint32_t handle, uint64_t bytes, bool made)
{
	if (leaks_on < 0) {
		const char *e = getenv("STARBITE_DRM_LEAKS");
		leaks_on = e == NULL ? 1 : *e == '0' ? 0 : *e == '2' ? 2 : 1;
	}
	if (!leaks_on || handle >= LEAK_HANDLES) {
		return;
	}
	void *pcs[LEAK_FRAMES + 3] = { NULL };
	if (made) {
		backtrace(pcs, LEAK_FRAMES + 3);
	}
	pthread_mutex_lock(&leak_lock);
	if (made) {
		unsigned i;
		for (i = 0; i < leak_nsites; i++) {
			if (memcmp(leak_sites[i].pcs, pcs + 3, sizeof(leak_sites[i].pcs)) == 0) {
				break;
			}
		}
		if (i == leak_nsites && leak_nsites < LEAK_SITES) {
			memcpy(leak_sites[i].pcs, pcs + 3, sizeof(leak_sites[i].pcs));
			leak_nsites++;
		}
		if (i < leak_nsites) {
			leak_sites[i].live_bytes += bytes;
			leak_sites[i].live++;
			leak_sites[i].made++;
			leak_handles[handle].bytes = bytes;
			leak_handles[handle].site = (uint16_t)i;
			leak_handles[handle].used = true;
		}
	} else if (leak_handles[handle].used) {
		struct leak_site *s = &leak_sites[leak_handles[handle].site];
		s->live_bytes -= leak_handles[handle].bytes;
		s->live--;
		leak_handles[handle].used = false;
	}
	time_t now = time(NULL);
	if (now - leak_last >= 15) {
		leak_last = now;
		leak_dump_locked();
	}
	pthread_mutex_unlock(&leak_lock);
}

/*
 * Requests counted by selector (0-15; 16 = GEM_CLOSE, 17 = others), and every 10 s one line on the kernel's console through
 * StarbiteMali's LOG selector: "STARLTE GPU APP: drm[pid] 10 s: submit N wait N madvise N(local) ...". Only with
 * STARBITE_DRM_STATS=1 in the environment (a clock read and two atomics a request otherwise saved).
 */
static uint64_t sel_count[18], sel_last_ns, mmap_local;

void
sbdrm_count_mmap_local(void)
{
	__atomic_fetch_add(&mmap_local, 1, __ATOMIC_RELAXED);
}

uint64_t sbdrm_flush_reason[10];                         /* pan_job.c: batches submitted, by why */
/* PanVK (mesa26-panvk-batch-stats.patch): batches with work by what closed them, their vertex/tiler/compute jobs, and those
 * with no fragment job. Plain counters, bumped without atomics by PanVK's recording threads: a statistic, not an account. */
unsigned sbdrm_vk_why;
unsigned long long sbdrm_vk_batches[10], sbdrm_vk_jobs, sbdrm_vk_fragless;
void
sbdrm_count(uint32_t selector)
{
	static int stats = -1;
	if (stats < 0) {
		stats = getenv("STARBITE_DRM_STATS") != NULL;
	}
	if (!stats) {
		return;
	}
	__atomic_fetch_add(&sel_count[selector < 16 ? selector : selector == SBM_GEM_CLOSE ? 16 : 17], 1, __ATOMIC_RELAXED);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	uint64_t last = __atomic_load_n(&sel_last_ns, __ATOMIC_RELAXED);
	if (last == 0) {
		__atomic_store_n(&sel_last_ns, now, __ATOMIC_RELAXED);
		return;
	}
	if (now - last < 10000000000ull || !__atomic_compare_exchange_n(&sel_last_ns, &last, now, false, __ATOMIC_RELAXED,
	    __ATOMIC_RELAXED)) {
		return;
	}
	uint64_t c[18];
	for (int i = 0; i < 18; i++) {
		c[i] = __atomic_exchange_n(&sel_count[i], 0, __ATOMIC_RELAXED);
	}
	char text[250];
	int n = snprintf(text, sizeof(text), "drm[%d] %llu s: submit %llu wait %llu create %llu mmap %llu param %llu "
	    "offset %llu madvise %llu(local) close %llu other %llu; mmap local %llu", getpid(), (unsigned long long)((now - last) / 1000000000ull),
	    c[0], c[1], c[2], c[3], c[4], c[5], c[8], c[16], c[6] + c[7] + c[9] + c[10] + c[11] + c[12] + c[13] + c[14] + c[15] + c[17],
	    (unsigned long long)__atomic_exchange_n(&mmap_local, 0, __ATOMIC_RELAXED));
	if (n > 0 && n < (int)sizeof(text)) {
		sbdrm_kernel_log(text, (size_t)n + 1);
	}
	unsigned long long vb[10], vall = 0;
	for (int i = 0; i < 10; i++) {
		vb[i] = __atomic_exchange_n(&sbdrm_vk_batches[i], 0, __ATOMIC_RELAXED);
		vall += vb[i];
	}
	if (vall != 0) {
		n = snprintf(text, sizeof(text), "drm[%d] vk batches %llu: end-rendering %llu barrier %llu before-dispatch %llu dispatch %llu "
		    "begin-rendering %llu job-limit %llu end-cmdbuf %llu event %llu query %llu other %llu; jobs %llu; no fragment job %llu",
		    getpid(), vall, vb[1], vb[2], vb[3], vb[4], vb[5], vb[6], vb[7], vb[8], vb[9], vb[0],
		    __atomic_exchange_n(&sbdrm_vk_jobs, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&sbdrm_vk_fragless, 0, __ATOMIC_RELAXED));
		if (n > 0) {
			text[sizeof(text) - 1] = 0;
			sbdrm_kernel_log(text, strlen(text) + 1);
		}
	}
	uint64_t f[10];
	for (int i = 0; i < 10; i++) {
		f[i] = __atomic_exchange_n(&sbdrm_flush_reason[i], 0, __ATOMIC_RELAXED);
	}
	n = snprintf(text, sizeof(text), "drm[%d] batches by why: lru %llu fresh %llu dep-writer %llu dep-readers %llu "
	    "flush-all %llu flush-writer %llu flush-users %llu; non-empty %llu; renamed %llu", getpid(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
	if (n > 0 && n < (int)sizeof(text)) {
		sbdrm_kernel_log(text, (size_t)n + 1);
	}
}
