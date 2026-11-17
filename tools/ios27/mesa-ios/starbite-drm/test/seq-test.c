// SPDX-License-Identifier: MIT-0
/*
 * The fences of starbite_drm.c (STARBITE_DRM_SEQ=1) against a stand-in for the kernel, on a Mac: the shim is compiled into
 * this program as it is, and its four IOKit calls are answered here by a FIFO of chains that a thread of this program
 * "runs" (a chain's address is how many microseconds it takes), with StarbiteMali's selectors as bsd/dev/starlte_gpu.c
 * and iokit/Kernel/arm/StarbiteMali.cpp answer them: SUBMIT and SUBMIT_SEQ queue, WAIT_IDLE waits for every chain queued
 * so far, WAIT_SEQ for one number, both for 3 s at most.
 *
 *   seq-test off      the switch off: every wait is a wait for the whole queue; the sync-file requests are refused (ENOSYS)
 *   seq-test on       the switch on: the cases below, then threads that submit and wait at random
 *   seq-test old      the switch on, a kernel without WAIT_SEQ: as off
 *
 * The property every case checks: a wait that says a fence is signalled never says it before that fence's chain is done.
 * And the point of the switch: a wait for an early chain does not wait for the chains queued after it.
 * tools/ios27/mesa-ios/starbite-drm/test/run.sh builds and runs all three.
 */
#include "../starbite_drm.c"
#include "../starbite_diag.c"

#include <assert.h>
#include <drm-uapi/panfrost_drm.h>

/* ---- the stand-in kernel ---- */
const mach_port_t kIOMainPortDefault = 0;
static pthread_mutex_t k_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t k_cond = PTHREAD_COND_INITIALIZER;
static uint64_t k_queue_us[100000];
static uint64_t k_seq, k_done;
static bool k_has_wait_seq = true;
static unsigned k_calls[0x110];

CFMutableDictionaryRef
IOServiceMatching(const char *name)
{
	(void)name;
	return NULL;
}

io_service_t
IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching)
{
	(void)port;
	(void)matching;
	return 1;
}

kern_return_t
IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect)
{
	(void)service;
	(void)task;
	(void)type;
	*connect = 1;
	return KERN_SUCCESS;
}

kern_return_t
IOObjectRelease(io_object_t object)
{
	(void)object;
	return KERN_SUCCESS;
}

static void *
k_worker(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&k_lock);
	for (;;) {
		while (k_done == k_seq) {
			pthread_cond_wait(&k_cond, &k_lock);
		}
		uint64_t us = k_queue_us[(k_done + 1) % 100000];
		pthread_mutex_unlock(&k_lock);
		usleep((useconds_t)us);
		pthread_mutex_lock(&k_lock);
		k_done++;
		pthread_cond_broadcast(&k_cond);
	}
	return NULL;
}

static uint64_t
k_enqueue(uint64_t us)
{
	pthread_mutex_lock(&k_lock);
	uint64_t seq = ++k_seq;
	k_queue_us[seq % 100000] = us;
	pthread_cond_broadcast(&k_cond);
	pthread_mutex_unlock(&k_lock);
	return seq;
}

/* until chain `seq` is done, for rel ns at most (capped at 3 s as the kernel does); true: done */
static bool
k_wait(uint64_t seq, int64_t rel)
{
	struct timespec now, until;
	clock_gettime(CLOCK_REALTIME, &now);
	if (rel > 3000000000ll || rel < 0) {
		rel = 3000000000ll;
	}
	until.tv_sec = now.tv_sec + (time_t)(rel / 1000000000ll);
	until.tv_nsec = now.tv_nsec + (long)(rel % 1000000000ll);
	if (until.tv_nsec >= 1000000000l) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000l;
	}
	pthread_mutex_lock(&k_lock);
	while (rel != 0 && k_done < seq) {
		if (pthread_cond_timedwait(&k_cond, &k_lock, &until) != 0) {
			break;
		}
	}
	bool done = k_done >= seq;
	pthread_mutex_unlock(&k_lock);
	return done;
}

kern_return_t
IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in, size_t in_size, void *out,
    size_t *out_size)
{
	(void)connection;
	(void)in_size;
	(void)out_size;
	__atomic_fetch_add(&k_calls[selector < 0x110 ? selector : 0x10f], 1, __ATOMIC_RELAXED);
	switch (selector) {
	case 0: {                                       /* SUBMIT */
		struct drm_panfrost_submit s;
		memcpy(&s, in, sizeof(s));
		(void)k_enqueue(s.jc);
		return KERN_SUCCESS;
	}
	case 0x107: {                                   /* SUBMIT_SEQ: the number back in place of jc */
		if (!k_has_wait_seq) {
			return kIOReturnUnsupported;
		}
		struct drm_panfrost_submit s;
		memcpy(&s, in, sizeof(s));
		s.jc = k_enqueue(s.jc);
		memcpy(out, &s, sizeof(s));
		return KERN_SUCCESS;
	}
	case 0x105:                                     /* ASYNC */
		*(uint32_t *)out = 1;
		return KERN_SUCCESS;
	case 0x106: {                                   /* WAIT_IDLE: everything queued so far */
		int64_t rel;
		memcpy(&rel, in, sizeof(rel));
		pthread_mutex_lock(&k_lock);
		uint64_t upto = k_seq;
		pthread_mutex_unlock(&k_lock);
		return k_wait(upto, rel) ? KERN_SUCCESS : kIOReturnTimeout;
	}
	case 0x108: {                                   /* WAIT_SEQ */
		if (!k_has_wait_seq) {
			return kIOReturnUnsupported;
		}
		struct sbm_wait_seq_req w;
		memcpy(&w, in, sizeof(w));
		pthread_mutex_lock(&k_lock);
		if (w.seq > k_seq) {
			w.seq = k_seq;
		}
		pthread_mutex_unlock(&k_lock);
		bool done = k_wait(w.seq, w.timeout_ns);
		pthread_mutex_lock(&k_lock);
		w.done = k_done;
		pthread_mutex_unlock(&k_lock);
		memcpy(out, &w, sizeof(w));
		return done ? KERN_SUCCESS : kIOReturnTimeout;
	}
	case 0x102:                                     /* LOG */
		printf("  [kernel log] %s\n", (const char *)in);
		return KERN_SUCCESS;
	default:
		return kIOReturnUnsupported;
	}
}

/* ---- the client side, as PanVK does it ---- */
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t
done_now(void)
{
	pthread_mutex_lock(&k_lock);
	uint64_t d = k_done;
	pthread_mutex_unlock(&k_lock);
	return d;
}

static uint64_t
queued_now(void)
{
	pthread_mutex_lock(&k_lock);
	uint64_t q = k_seq;
	pthread_mutex_unlock(&k_lock);
	return q;
}

/* a chain of `us` microseconds signalling `out_sync`; its number (the stand-in's count) */
static uint64_t
submit(uint32_t out_sync, uint64_t us)
{
	struct drm_panfrost_submit s = { .jc = us, .out_sync = out_sync };
	int ret = drmIoctl(-1, DRM_IOCTL_PANFROST_SUBMIT, &s);
	CHECK(ret == 0, "submit: %d", ret);
	CHECK(s.jc == us, "submit changed the caller's jc");
	return queued_now();
}

/* panvk_queue_transfer_sync(): the queue's fence into `syncobj` through a sync file */
static void
transfer(uint32_t queue_sync, uint32_t syncobj)
{
	struct drm_syncobj_handle h = { .handle = queue_sync, .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, .fd = -1 };
	int ret = drmIoctl(-1, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h);
	CHECK(ret == 0 && h.fd >= 0, "export: %d fd %d", ret, h.fd);
	h.handle = syncobj;
	h.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	ret = drmIoctl(-1, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h);
	CHECK(ret == 0, "import: %d", ret);
	close(h.fd);
}

static int
wait1(uint32_t syncobj, int64_t rel_ns)
{
	return drmSyncobjWait(-1, &syncobj, 1, rel_ns == INT64_MAX ? INT64_MAX : (int64_t)now_ns() + rel_ns, 0, NULL);
}

static void
cases_on(void)
{
	uint32_t q, f, sig;
	drmSyncobjCreate(-1, DRM_SYNCOBJ_CREATE_SIGNALED, &q);
	drmSyncobjCreate(-1, 0, &f);
	drmSyncobjCreate(-1, DRM_SYNCOBJ_CREATE_SIGNALED, &sig);
	CHECK(seq_on, "the switch did not come on");

	/* 1: a fence for chain A is signalled when A is done, not when the B queued after it is */
	unsigned idle0 = k_calls[0x106];
	uint64_t a = submit(q, 20000);
	transfer(q, f);
	uint64_t b = submit(q, 300000);
	uint64_t t0 = now_ns();
	CHECK(wait1(f, INT64_MAX) == 0, "wait for A");
	uint64_t ms = (now_ns() - t0) / 1000000, d = done_now();
	CHECK(d >= a, "case 1: returned before A was done (done %llu, A %llu)", d, a);
	CHECK(d < b && ms < 200, "case 1: waited for B too (%llu ms, done %llu, B %llu)", ms, d, b);
	CHECK(k_calls[0x106] == idle0, "case 1: a whole-queue wait was made");
	printf("case 1: fence of A after %llu ms, B not done (done %llu of %llu)\n", ms, d, b);

	/* 2: the queue's own syncobj holds B */
	CHECK(wait1(q, INT64_MAX) == 0 && done_now() >= b, "case 2: q before B was done");

	/* 3: reset (no fence): as before, the whole queue */
	drmSyncobjReset(-1, &f, 1);
	uint64_t c = submit(q, 50000);
	unsigned idle1 = k_calls[0x106];
	CHECK(wait1(f, INT64_MAX) == 0, "case 3 wait");
	CHECK(k_calls[0x106] == idle1 + 1 && done_now() >= c, "case 3: a fence-less wait was not a whole-queue wait");

	/* 4: signalled: no kernel call */
	unsigned seqw = k_calls[0x108];
	submit(q, 100000);
	CHECK(wait1(sig, INT64_MAX) == 0 && k_calls[0x108] == seqw && k_calls[0x106] == idle1 + 1,
	    "case 4: a signalled fence asked the kernel");
	CHECK(wait1(q, INT64_MAX) == 0, "case 4 drain");

	/* 5: a timeout comes back as ETIME, after the time asked */
	uint32_t f2;
	drmSyncobjCreate(-1, 0, &f2);
	uint64_t e = submit(q, 300000);
	transfer(q, f2);
	t0 = now_ns();
	int ret = wait1(f2, 10000000);
	ms = (now_ns() - t0) / 1000000;
	CHECK(ret == -ETIME && errno == ETIME && ms < 150 && done_now() < e, "case 5: %d errno %d after %llu ms", ret, errno, ms);
	CHECK(wait1(f2, 0) == -ETIME, "case 5: a look at a running chain said done");
	CHECK(wait1(f2, INT64_MAX) == 0 && done_now() >= e, "case 5 drain");
	CHECK(wait1(f2, 0) == 0, "case 5: a look at a done chain said running");

	/* 6, 7: any and all */
	uint32_t f3, f4, two[2];
	drmSyncobjCreate(-1, 0, &f3);
	drmSyncobjCreate(-1, 0, &f4);
	uint64_t ds = submit(q, 10000);
	transfer(q, f3);
	uint64_t es = submit(q, 300000);
	transfer(q, f4);
	two[0] = f4;
	two[1] = f3;
	uint32_t first = 99;
	ret = drmSyncobjWait(-1, two, 2, INT64_MAX, 0, &first);
	d = done_now();
	CHECK(ret == 0 && first == 1 && d >= ds && d < es, "case 6: any: %d first %u done %llu (D %llu E %llu)", ret, first, d, ds, es);
	ret = drmSyncobjWait(-1, two, 2, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, &first);
	CHECK(ret == 0 && done_now() >= es, "case 7: all before E was done");

	/* 8, 9: timeline waits and queries: as before */
	unsigned idle2 = k_calls[0x106];
	uint64_t point = 1;
	submit(q, 20000);
	CHECK(drmSyncobjTimelineWait(-1, &f3, &point, 1, INT64_MAX, 0, NULL) == 0 && k_calls[0x106] == idle2 + 1,
	    "case 8: a timeline wait was not a whole-queue wait");
	CHECK(drmSyncobjQuery(-1, &f3, &point, 1) == 0 && k_calls[0x106] == idle2 + 2, "case 9: a query was not a whole-queue wait");

	/* 10: a timeline transfer, an unknown handle: as before */
	drmSyncobjTransfer(-1, f3, 5, q, 0, 0);
	unsigned idle3 = k_calls[0x106];
	CHECK(wait1(f3, INT64_MAX) == 0 && k_calls[0x106] == idle3 + 1, "case 10: a timeline point was followed");
	CHECK(wait1(123456, INT64_MAX) == 0 && k_calls[0x106] == idle3 + 2, "case 10: an unknown handle was followed");

	/* 11: drmSyncobjTransfer (binary) and drmSyncobjSignal */
	uint64_t g = submit(q, 30000);
	drmSyncobjTransfer(-1, f3, 0, q, 0, 0);
	submit(q, 300000);
	CHECK(wait1(f3, INT64_MAX) == 0 && done_now() >= g && done_now() < g + 1 + 1, "case 11: transfer");
	drmSyncobjSignal(-1, &f4, 1);
	seqw = k_calls[0x108];
	CHECK(wait1(f4, INT64_MAX) == 0 && k_calls[0x108] == seqw, "case 11: a signalled fence asked the kernel");
	CHECK(wait1(q, INT64_MAX) == 0, "case 11 drain");
}

/* 12: threads that submit and wait at random; no wait may come back before its own chain is done */
#define THREADS 6
#define ROUNDS 400
static uint32_t shared_q;
static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;

static void *
stress(void *arg)
{
	unsigned r = (unsigned)(uintptr_t)arg * 2654435761u;
	uint32_t mine;
	drmSyncobjCreate(-1, 0, &mine);
	for (int i = 0; i < ROUNDS; i++) {
		r = r * 1103515245u + 12345u;
		uint64_t us = 50 + (r >> 8) % 1500;
		/* (PanVK: the queue's submit and the transfer under the queue's lock) */
		pthread_mutex_lock(&submit_lock);
		uint64_t s = submit(shared_q, us);
		transfer(shared_q, mine);
		pthread_mutex_unlock(&submit_lock);
		int64_t rel = (r >> 4) % 3 == 0 ? 0 : (r >> 4) % 3 == 1 ? 200000 : INT64_MAX;
		int ret = wait1(mine, rel);
		uint64_t d = done_now();
		if (ret == 0) {
			CHECK(d >= s, "stress: signalled before its chain (done %llu, chain %llu)", d, s);
		} else {
			CHECK(ret == -ETIME, "stress: %d", ret);
		}
		if (r % 7 == 0) {
			drmSyncobjReset(-1, &mine, 1);
		}
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "on";
	pthread_t worker;
	pthread_create(&worker, NULL, k_worker, NULL);
	if (strcmp(mode, "off") != 0) {
		setenv("STARBITE_DRM_SEQ", "1", 1);
	}
	k_has_wait_seq = strcmp(mode, "old") != 0;
	unsetenv("STARBITE_DRM_STATS");
	setenv("STARBITE_DRM_LEAKS", "0", 1);

	if (strcmp(mode, "on") == 0) {
		cases_on();
		drmSyncobjCreate(-1, DRM_SYNCOBJ_CREATE_SIGNALED, &shared_q);
		pthread_t t[THREADS];
		for (uintptr_t i = 0; i < THREADS; i++) {
			pthread_create(&t[i], NULL, stress, (void *)(i + 1));
		}
		for (int i = 0; i < THREADS; i++) {
			pthread_join(t[i], NULL);
		}
		printf("case 12: %d threads x %d rounds, whole-queue waits %u, by number %u\n", THREADS, ROUNDS, k_calls[0x106],
		    k_calls[0x108]);
	} else {
		uint32_t q, f;
		drmSyncobjCreate(-1, DRM_SYNCOBJ_CREATE_SIGNALED, &q);
		drmSyncobjCreate(-1, 0, &f);
		CHECK(!seq_on, "%s: the switch came on", mode);
		uint64_t a = submit(q, 20000);
		struct drm_syncobj_handle h = { .handle = q, .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, .fd = -1 };
		int ret = drmIoctl(-1, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h);
		CHECK(ret == -1 && errno == ENOSYS, "%s: the sync-file request was answered (%d, errno %d)", mode, ret, errno);
		uint64_t b = submit(q, 100000);
		unsigned idle = k_calls[0x106];
		CHECK(wait1(f, INT64_MAX) == 0 && done_now() >= b && done_now() >= a, "%s: not a whole-queue wait", mode);
		CHECK(k_calls[0x106] == idle + 1 && k_calls[0x108] == (strcmp(mode, "old") == 0 ? 1u : 0u) && k_calls[0x107] == 0,
		    "%s: calls whole %u seq %u submit_seq %u", mode, k_calls[0x106] - idle, k_calls[0x108], k_calls[0x107]);
		printf("%s: waits as before (whole queue), submits as before\n", mode);
	}
	printf("%s: %s (%d failures)\n", mode, failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
