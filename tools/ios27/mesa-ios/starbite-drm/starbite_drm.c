/*
 * Project Starbite: the libdrm of xf86drm.h (see there) for Mesa's Panfrost
 * on iOS. Panfrost's DRM requests go to the kernel's StarbiteMali user client
 * as IOConnectCallStructMethod calls:
 *
 *   DRM_COMMAND_BASE + n (Panfrost's SUBMIT .. MADVISE)  -> selector n
 *   DRM_IOCTL_GEM_CLOSE                                   -> selector 0x100
 *
 * with the request's structure going in and coming back as it is. The
 * kernel maps a buffer into this process itself when asked for its "mmap
 * offset": the offset Mesa gets back is the address (Mesa's pan_kmod_bo_mmap
 * is patched to use it as such, tools/ios27/mesa-ios/panfrost-ios.patch).
 *
 * Panfrost knows its device by a file descriptor. It gets one that means
 * nothing (/dev/null): there is one GPU and one connection to it in a
 * process.
 *
 * Fences: STARBITE_DRM_SEQ=1 makes a fence wait a wait for the fence's own
 * chain instead of for the whole queue (seq_wait() below; off by default).
 * STARBITE_DRM_FENCES=1 is the same switch under the name the 12 November
 * shim (built into the PanVK of phone 1's c2 bundle) gave its own version.
 *
 * After HakiOS tools/mesa/hakios-drm/hakios.drm.c (MIT, Copyright the HakiOS
 * authors), reworked for Panfrost and StarbiteMali.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/ioccom.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define SBM_SERVICE     "StarbiteMali"
#define SBM_GEM_CLOSE   0x100
#define SBM_SURFACE     0x103
#define SBM_IMPORT_HOST 0x104
#define SBM_ASYNC       0x105
#define SBM_WAIT_IDLE   0x106

static io_connect_t connection;
static int connection_fd = -1;
static uint32_t next_syncobj;
/*
 * Asynchronous submits. The kernel (boot-arg sweetspeed&64) starts a chain and returns; a
 * fence (syncobj) wait is then a wait for the GPU to be done with everything submitted so far (the
 * kernel runs chains one at a time, in order), WAIT_BO waits for the chains that named the buffer.
 * STARBITE_DRM_SYNC=1: synchronous submits as before.
 */
static bool async_on;
static bool seq_on;
static void seq_probe(void);

/*
 * The connection to StarbiteMali, opened at the first request whoever makes it: Panfrost GL comes
 * through drmOpenWithType(), PanVK opens the device node of drmGetDevices2() itself.
 */
/* One line on the console (/dev/console, not the kernel's LOG call: it is said before or without a connection). */
static void
console_say(const char *format, ...)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		return;
	}
	char text[200];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	if (n > 0) {
		dprintf(fd, "SBDRM[%d %s]: %s\n", getpid(), getprogname(), text);
	}
	close(fd);
}

static bool
starbite_connect(void)
{
	if (connection != IO_OBJECT_NULL) {
		return true;
	}
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SBM_SERVICE));
	kern_return_t kr = KERN_FAILURE;
	if (service != IO_OBJECT_NULL) {
		kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
		IOObjectRelease(service);
	}
	/* once a process, and at every failure: whether the GPU was reached (a process that never draws says why here) */
	static int said;
	if (!said || kr != KERN_SUCCESS) {
		said = 1;
		console_say("StarbiteMali %s, IOServiceOpen -> %#x", service != IO_OBJECT_NULL ? "found" : "NOT FOUND", kr);
	}
	if (kr != KERN_SUCCESS) {
		return false;
	}
	if (getenv("STARBITE_DRM_SYNC") == NULL) {
		uint32_t flag = 1;
		size_t returned = sizeof(flag);
		if (IOConnectCallStructMethod(connection, SBM_ASYNC, &flag, sizeof(flag), &flag, &returned) == KERN_SUCCESS &&
		    flag != 0) {
			async_on = true;
		}
	}
	seq_probe();
	console_say("async submits %s, fences by chain number %s", async_on ? "on" : "off",
	    seq_on ? "on (STARBITE_DRM_SEQ=1 or STARBITE_DRM_FENCES=1)" : "off");
	return true;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name == NULL || strcmp(name, "panfrost") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	if (!starbite_connect()) {
		return -1;
	}
	connection_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	return dup(connection_fd);
}

/* STARBITE_DRM_TRACE=1: the first 300 calls and every failed one on stderr. */
static int trace = -1;

static void
trace_call(uint32_t selector, const void *data, size_t size, kern_return_t kr)
{
	const uint32_t *w = data;
	const uint64_t *q = data;

	if (selector == 0 && size >= 48) {             /* SUBMIT: jc, bo count, requirements */
		fprintf(stderr, "starbite-drm: SUBMIT jc %#llx bos %u req %#x -> %#x\n",
		    (unsigned long long)q[0], w[8], w[9], kr);
	} else if (selector == 2 && size >= 16) {      /* CREATE_BO: size, flags -> handle, va */
		fprintf(stderr, "starbite-drm: CREATE_BO size %#x flags %#x -> handle %u va %#llx (%#x)\n",
		    w[0], w[1], w[2], (unsigned long long)q[2], kr);
	} else {
		fprintf(stderr, "starbite-drm: sel %#x size %zu first %#x -> %#x\n", selector, size,
		    size >= 4 ? w[0] : 0, kr);
	}
}


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
	size_t returned = 0;
	if (leaks_on == 2) {
		int n = snprintf(con, sizeof(con), "SBDRM %s[%d] live %llu MiB in %llu buffers", getprogname(), getpid(),
		        (unsigned long long)(all >> 20), (unsigned long long)count);
		(void)IOConnectCallStructMethod(connection, 0x102, con, (size_t)n + 1, NULL, &returned);
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
			(void)IOConnectCallStructMethod(connection, 0x102, con, strlen(con) + 1, NULL, &returned);
		}
	}
	fclose(f);
}

static void
leak_note(uint32_t handle, uint64_t bytes, bool made)
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

static int
call(uint32_t selector, void *data, size_t size)
{
	size_t returned = size;
	if (!starbite_connect()) {
		return ENODEV;
	}
	kern_return_t kr = IOConnectCallStructMethod(connection, selector, data, size, data, &returned);

	if (trace < 0) {
		const char *t = getenv("STARBITE_DRM_TRACE");
		trace = t != NULL && *t == '1';
	}
	static unsigned traced;
	if (trace && (traced++ < 300 || kr != KERN_SUCCESS)) {
		trace_call(selector, data, size, kr);
	}

	if (kr == KERN_SUCCESS) {
		const uint32_t *w = data;
		const uint64_t *q = data;
		if (selector == 2 && size >= 16) {                     /* CREATE_BO: size, flags -> handle */
			leak_note(w[2], w[0], true);
		} else if (selector == SBM_IMPORT_HOST && size >= 20) { /* addr, size -> handle */
			leak_note(w[4], q[1], true);
		} else if (selector == SBM_GEM_CLOSE && size >= 4) {
			leak_note(w[0], 0, false);
		}
	}

	switch (kr) {
	case KERN_SUCCESS:
		return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoSpace:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

/*
 * Fences that stand for their own chains: STARBITE_DRM_SEQ=1 in the environment (for the window server, a line in
 * /private/var/local/mtl2-env). Off by default; without it nothing below changes what any call does.
 *
 * Without it a fence (syncobj) wait is a wait for every chain queued at the moment of the wait (SBM_WAIT_IDLE). A thread
 * that waits for frame N's command buffer then also waits for the chains of frame N+1 that the render thread queued in
 * the meantime, and the window server's CPU work and the GPU's take turns instead of overlapping. On the S9 on 17 November
 * 2026, swiping the home screen under the Metal compositor, the kernel counted 664 such waits in 10 s at 9.2 ms each.
 *
 * With it a submit asks the kernel for its chain's number (SBM_SUBMIT_SEQ, 0x107: the same submit, the number comes back
 * in place of the chain's address), the syncobj it signals keeps that number, and a wait on syncobjs whose fences are all
 * known is a wait for that number (SBM_WAIT_SEQ, 0x108). The kernel runs the chains of every client one at a time in the
 * order they were queued (bsd/dev/starlte_gpu.c), so a chain's number being done means every chain queued before it is
 * done as well: what a wait for that fence is owed, and no more.
 *
 * What a syncobj holds here, followed as the kernel's syncobj would hold a fence:
 *   SEQ_NONE       no fence: made unsignalled, or reset. A wait falls back to SBM_WAIT_IDLE, as without the switch. (A real
 *                  syncobj fails such a wait, or with WAIT_FOR_SUBMIT blocks until a fence is attached; PanVK submits on its
 *                  queue's syncobj and only then moves that fence into the caller's fences and semaphores, so in between a
 *                  waiter keeps today's answer, which covers the chain just queued.)
 *   SEQ_SIGNALLED  a signalled fence: made signalled, signalled by the CPU, or imported from a sync file that was signalled
 *   SEQ_CHAIN      the fence of chain `seq`: a submit's out_sync, or moved from one (transfer, or a sync file exported and
 *                  imported, which is how PanVK's queue_transfer_sync moves it)
 *   SEQ_UNKNOWN    anything not followed exactly (a timeline point moved or signalled, a handle that found no room, a sync
 *                  file this process did not export): SBM_WAIT_IDLE, as without the switch
 * Timeline waits and queries (drmSyncobjTimelineWait, drmSyncobjQuery2) are left as they are: telling a point not attached
 * yet from one that is needs the points followed, which this does not do (drmGetCap says there are no timelines). So a
 * wait never ends before the fence it was asked about has; it only stops waiting for chains queued after that fence.
 *
 * The sync-file requests (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD with EXPORT_SYNC_FILE, FD_TO_HANDLE with IMPORT_SYNC_FILE) and
 * DRM_IOCTL_SYNCOBJ_RESET, which PanVK sends through drmIoctl, are answered (ENOSYS without the switch, as before): the
 * file is /dev/null as drmSyncobjExportSyncFile's always was, and what it stood for is remembered by its number until the
 * import. The only sync files in a process are the ones made here.
 *
 * A line every 10 s on the kernel's console (StarbiteMali's LOG): "drm[pid] fences 10 s: waits by number N (already done
 * D, timed out T), as before W (no fence F, not followed U); submits numbered S". Its kernel half is "STARLTE GPU CALLS"
 * (wait count and time) and, with boot-arg sbm_prof=1, "STARLTE GPU WAITS" (whole queue against one chain, by thread).
 * NOT run on a phone when this was written.
 */
#define SBM_SUBMIT_SEQ  0x107
#define SBM_WAIT_SEQ    0x108
#define SEQ_SLOTS       4096u
#define SEQ_FDS         1024
enum { SEQ_NONE = 1, SEQ_SIGNALLED, SEQ_CHAIN, SEQ_UNKNOWN };
struct seq_fence {
	uint32_t state;
	uint64_t seq;
};
static struct {
	uint32_t handle;
	struct seq_fence fence;
} seq_table[SEQ_SLOTS];
static struct seq_fence seq_fds[SEQ_FDS];               /* exported sync files, by descriptor, until imported */
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t seq_done_seen;                          /* the last chain number the kernel has said is done */
enum { SEQ_ST_BY_NUMBER, SEQ_ST_ALREADY, SEQ_ST_TIMEOUT, SEQ_ST_AS_BEFORE, SEQ_ST_NO_FENCE, SEQ_ST_UNFOLLOWED,
	SEQ_ST_SUBMITS, SEQ_ST_COUNT };
static uint64_t seq_stat[SEQ_ST_COUNT], seq_stat_since;
static int call(uint32_t selector, void *data, size_t size);
struct sbm_wait_seq_req {
	uint64_t seq;
	int64_t timeout_ns;                             /* relative; 0: only look */
	uint64_t done;                                  /* out: the last chain done */
};

/* The switch: STARBITE_DRM_SEQ=1, or STARBITE_DRM_FENCES=1 (the 12 November shim's name for its version of it). */
static bool
seq_wanted(void)
{
	const char *e = getenv("STARBITE_DRM_SEQ"), *f = getenv("STARBITE_DRM_FENCES");
	return (e != NULL && strcmp(e, "1") == 0) || (f != NULL && strcmp(f, "1") == 0);
}

/* Once the connection is open (starbite_connect): the switch read, and the kernel asked whether it has WAIT_SEQ. */
static void
seq_probe(void)
{
	if (!seq_wanted()) {
		return;
	}
	struct sbm_wait_seq_req w = { 0, 0, 0 };
	size_t returned = sizeof(w);
	bool have = async_on && IOConnectCallStructMethod(connection, SBM_WAIT_SEQ, &w, sizeof(w), &w, &returned) == KERN_SUCCESS;
	char text[160];
	int n = snprintf(text, sizeof(text), "SBDRM[%d %s]: fences by chain number %s", getpid(), getprogname(),
	    have ? "on (STARBITE_DRM_SEQ=1)" : async_on ? "NOT on: the kernel has no WAIT_SEQ; waits as before" :
	    "NOT on: submits are synchronous; waits as before");
	if (n > 0 && n < (int)sizeof(text)) {
		size_t none = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &none);
	}
	seq_on = have;
}

static void
seq_count(unsigned what)
{
	__atomic_fetch_add(&seq_stat[what], 1, __ATOMIC_RELAXED);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	uint64_t since = __atomic_load_n(&seq_stat_since, __ATOMIC_RELAXED);
	if (since == 0) {
		__atomic_store_n(&seq_stat_since, now, __ATOMIC_RELAXED);
		return;
	}
	if (now - since < 10000000000ull || !__atomic_compare_exchange_n(&seq_stat_since, &since, now, false, __ATOMIC_RELAXED,
	    __ATOMIC_RELAXED)) {
		return;
	}
	uint64_t c[SEQ_ST_COUNT];
	for (int i = 0; i < SEQ_ST_COUNT; i++) {
		c[i] = __atomic_exchange_n(&seq_stat[i], 0, __ATOMIC_RELAXED);
	}
	char text[200];
	int n = snprintf(text, sizeof(text), "drm[%d] fences %llu s: waits by number %llu (already done %llu, timed out %llu), "
	    "as before %llu (no fence %llu, not followed %llu); submits numbered %llu", getpid(),
	    (unsigned long long)((now - since) / 1000000000ull), (unsigned long long)c[SEQ_ST_BY_NUMBER],
	    (unsigned long long)c[SEQ_ST_ALREADY], (unsigned long long)c[SEQ_ST_TIMEOUT], (unsigned long long)c[SEQ_ST_AS_BEFORE],
	    (unsigned long long)c[SEQ_ST_NO_FENCE], (unsigned long long)c[SEQ_ST_UNFOLLOWED], (unsigned long long)c[SEQ_ST_SUBMITS]);
	if (n > 0 && n < (int)sizeof(text)) {
		size_t none = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &none);
	}
}

/* A handle's fence (SEQ_UNKNOWN for a handle not in the table); seq_lock held. */
static struct seq_fence
seq_get_locked(uint32_t handle)
{
	unsigned i = handle % SEQ_SLOTS;
	if (handle != 0 && seq_table[i].handle == handle) {
		return seq_table[i].fence;
	}
	return (struct seq_fence){ SEQ_UNKNOWN, 0 };
}

/* Set a handle's fence (nothing for a handle not in the table: it stays unknown); seq_lock held. */
static void
seq_set_locked(uint32_t handle, struct seq_fence fence)
{
	unsigned i = handle % SEQ_SLOTS;
	if (handle != 0 && seq_table[i].handle == handle) {
		seq_table[i].fence = fence;
	}
}

static void
seq_set(uint32_t handle, uint32_t state, uint64_t seq)
{
	if (!seq_on) {
		return;
	}
	pthread_mutex_lock(&seq_lock);
	seq_set_locked(handle, (struct seq_fence){ state, seq });
	pthread_mutex_unlock(&seq_lock);
}

/* A new handle: into the table when its slot is free (else it stays unknown: waits on it are as before). */
static void
seq_new(uint32_t handle, uint32_t state)
{
	if (!seq_on && seq_wanted()) {
		(void)starbite_connect();               /* (made before any other request: the switch is decided there) */
	}
	if (!seq_on || handle == 0) {
		return;
	}
	pthread_mutex_lock(&seq_lock);
	unsigned i = handle % SEQ_SLOTS;
	if (seq_table[i].handle == 0) {
		seq_table[i].handle = handle;
		seq_table[i].fence = (struct seq_fence){ state, 0 };
	}
	pthread_mutex_unlock(&seq_lock);
}

static void
seq_forget(uint32_t handle)
{
	if (!seq_on) {
		return;
	}
	pthread_mutex_lock(&seq_lock);
	unsigned i = handle % SEQ_SLOTS;
	if (handle != 0 && seq_table[i].handle == handle) {
		seq_table[i].handle = 0;
	}
	pthread_mutex_unlock(&seq_lock);
}

/* A submit's chain number for the syncobj it signals (0: no number, the chain is done: a signalled fence). */
static void
seq_submitted(uint32_t out_sync, uint64_t seq)
{
	seq_count(SEQ_ST_SUBMITS);
	if (out_sync == 0) {
		return;
	}
	pthread_mutex_lock(&seq_lock);
	struct seq_fence was = seq_get_locked(out_sync);
	if (seq == 0) {
		seq_set_locked(out_sync, (struct seq_fence){ SEQ_SIGNALLED, 0 });
	} else if (was.state != SEQ_CHAIN || was.seq < seq) {
		/* (two threads on one syncobj: the later chain is the one it holds, as the kernel's would) */
		seq_set_locked(out_sync, (struct seq_fence){ SEQ_CHAIN, seq });
	}
	pthread_mutex_unlock(&seq_lock);
}

/*
 * A wait by chain number. *handled false: some fence is not known exactly, and the caller waits as before. Otherwise 0
 * or ETIMEDOUT, and *first_signaled for a wait for any one of them.
 */
static int
seq_wait(const uint32_t *handles, unsigned n, int64_t rel, unsigned flags, uint32_t *first_signaled, bool *handled)
{
	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0 || n <= 1;
	uint64_t target = 0;
	uint32_t first = 0;
	bool ready = false;

	*handled = false;
	pthread_mutex_lock(&seq_lock);
	for (unsigned i = 0; i < n; i++) {
		struct seq_fence f = seq_get_locked(handles[i]);
		if (f.state == SEQ_NONE || f.state == SEQ_UNKNOWN) {
			pthread_mutex_unlock(&seq_lock);
			seq_count(f.state == SEQ_NONE ? SEQ_ST_NO_FENCE : SEQ_ST_UNFOLLOWED);
			seq_count(SEQ_ST_AS_BEFORE);
			return 0;
		}
		uint64_t s = f.state == SEQ_CHAIN ? f.seq : 0;
		if (all) {
			target = s > target ? s : target;
		} else if (s == 0 || s <= seq_done_seen) {
			ready = true;                   /* (one is signalled already: that one) */
			first = i;
			break;
		} else if (target == 0 || s < target) {
			target = s;                     /* (in order: the lowest number is done first) */
			first = i;
		}
	}
	bool done = ready || target == 0 || target <= seq_done_seen;
	pthread_mutex_unlock(&seq_lock);
	*handled = true;
	if (first_signaled != NULL) {
		*first_signaled = all ? 0 : first;
	}
	seq_count(SEQ_ST_BY_NUMBER);
	if (done) {
		seq_count(SEQ_ST_ALREADY);
		return 0;
	}
	struct sbm_wait_seq_req w = { target, rel > 0 ? rel : 0, 0 };
	int error = call(SBM_WAIT_SEQ, &w, sizeof(w));
	if (error == 0 || error == ETIMEDOUT) {
		pthread_mutex_lock(&seq_lock);
		if (w.done > seq_done_seen) {
			seq_done_seen = w.done;
		}
		pthread_mutex_unlock(&seq_lock);
	}
	if (error == ETIMEDOUT) {
		seq_count(SEQ_ST_TIMEOUT);
	}
	return error;
}

/* drmIoctl's sync-file and reset requests (only with the switch on). -2: not one of them. */
static int
seq_ioctl(unsigned long request, void *arg)
{
	if (request == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) {
		struct drm_syncobj_handle *h = arg;
		if (!(h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
			return -2;                      /* (a syncobj as a file: not here, as before) */
		}
		return drmSyncobjExportSyncFile(-1, h->handle, &h->fd);
	}
	if (request == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE) {
		struct drm_syncobj_handle *h = arg;
		if (!(h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) {
			return -2;
		}
		return drmSyncobjImportSyncFile(-1, h->handle, h->fd);
	}
	if (request == DRM_IOCTL_SYNCOBJ_RESET) {
		struct drm_syncobj_array *a = arg;
		return drmSyncobjReset(-1, (const uint32_t *)(uintptr_t)a->handles, a->count_handles);
	}
	return -2;
}

/*
 * Requests counted by selector (0-15; 16 = GEM_CLOSE, 17 = others), and every 10 s one line on the kernel's console through
 * StarbiteMali's LOG selector: "STARLTE GPU APP: drm[pid] 10 s: submit N wait N madvise N(local) ...". Only with
 * STARBITE_DRM_STATS=1 in the environment (a clock read and two atomics a request otherwise saved).
 * MADVISE is answered here: StarbiteMali never purges a buffer (its MADVISE only checks the handle and says
 * "retained"), and Panfrost's BO cache asks twice per cached buffer reuse. STARBITE_DRM_KMADVISE=1 asks the kernel again.
 */
static uint64_t sel_count[18], sel_last_ns, mmap_local;
uint64_t sbdrm_flush_reason[10];                         /* pan_job.c: batches submitted, by why */
/* PanVK (mesa26-panvk-batch-stats.patch): batches with work by what closed them, their vertex/tiler/compute jobs, and those
 * with no fragment job. Plain counters, bumped without atomics by PanVK's recording threads: a statistic, not an account. */
unsigned sbdrm_vk_why;
unsigned long long sbdrm_vk_batches[10], sbdrm_vk_jobs, sbdrm_vk_fragless;
#define MMAP_CACHE 8193u                                /* StarbiteMali's handles are 1..8192 */
static uint64_t mmap_cache[MMAP_CACHE];
static void
count_and_report(uint32_t selector)
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
		size_t returned = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &returned);
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
			size_t returned = 0;
			text[sizeof(text) - 1] = 0;
			(void)IOConnectCallStructMethod(connection, 0x102, text, strlen(text) + 1, NULL, &returned);
		}
	}
	uint64_t f[10];
	for (int i = 0; i < 10; i++) {
		f[i] = __atomic_exchange_n(&sbdrm_flush_reason[i], 0, __ATOMIC_RELAXED);
	}
	n = snprintf(text, sizeof(text), "drm[%d] batches by why: lru %llu fresh %llu dep-writer %llu dep-readers %llu "
	    "flush-all %llu flush-writer %llu flush-users %llu; non-empty %llu; renamed %llu", getpid(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
	if (n > 0 && n < (int)sizeof(text)) {
		size_t returned = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &returned);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	uint32_t nr = (uint32_t)(request & 0xff);
	uint32_t selector;

	(void)fd;
	if (seq_on) {
		int answered = seq_ioctl(request, arg);
		if (answered != -2) {
			return answered;
		}
	}
	static int kmadvise = -1;
	if (kmadvise < 0) {
		kmadvise = getenv("STARBITE_DRM_KMADVISE") != NULL;
	}
	if (nr >= DRM_COMMAND_BASE && nr - DRM_COMMAND_BASE == 8 && IOCPARM_LEN(request) >= 12 && !kmadvise) {
		((uint32_t *)arg)[2] = 1;                       /* MADVISE: retained */
		count_and_report(8);
		return 0;
	}
	/*
	 * MMAP_BO's answer is the kernel's mapping of the buffer, made once and kept until GEM_CLOSE
	 * (StarbiteMali mmapBO); Panfrost asks again every time it takes a buffer out of its cache. Remember it per handle.
	 */
	if (nr >= DRM_COMMAND_BASE && nr - DRM_COMMAND_BASE == 3 && IOCPARM_LEN(request) >= 16) {
		uint32_t h = ((uint32_t *)arg)[0];
		uint64_t at = h < MMAP_CACHE ? __atomic_load_n(&mmap_cache[h], __ATOMIC_RELAXED) : 0;
		if (at != 0) {
			((uint64_t *)arg)[1] = at;
			__atomic_fetch_add(&mmap_local, 1, __ATOMIC_RELAXED);
			count_and_report(3);
			return 0;
		}
	}
	if (request == DRM_IOCTL_GEM_CLOSE) {
		uint32_t h = ((uint32_t *)arg)[0];
		if (h < MMAP_CACHE) {
			__atomic_store_n(&mmap_cache[h], 0, __ATOMIC_RELAXED);
		}
	}
	if (nr >= DRM_COMMAND_BASE) {
		selector = nr - DRM_COMMAND_BASE;
	} else if (request == DRM_IOCTL_GEM_CLOSE) {
		selector = SBM_GEM_CLOSE;
	} else {
		if (getenv("STARBITE_DRM_TRACE") != NULL) {
			fprintf(stderr, "starbite-drm: ioctl %#lx not supported\n", request);
		}
		errno = ENOSYS;
		return -1;
	}
	count_and_report(selector);
	if (seq_on && selector == 0 && IOCPARM_LEN(request) >= 40 && IOCPARM_LEN(request) <= 64) {
		/* SUBMIT as SUBMIT_SEQ: the same submit; the chain's number comes back where its address was (a copy: Mesa's stays) */
		uint8_t copy[64];
		uint64_t seq;
		uint32_t out_sync;
		memcpy(copy, arg, IOCPARM_LEN(request));
		int error = call(SBM_SUBMIT_SEQ, copy, IOCPARM_LEN(request));
		if (error != 0) {
			errno = error;
			return -1;
		}
		memcpy(&seq, copy, sizeof(seq));
		memcpy(&out_sync, (const uint8_t *)arg + 20, sizeof(out_sync));  /* struct drm_panfrost_submit's out_sync */
		seq_submitted(out_sync, seq);
		return 0;
	}
	int error = call(selector, arg, IOCPARM_LEN(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	if (selector == 3 && IOCPARM_LEN(request) >= 16 && ((uint32_t *)arg)[0] < MMAP_CACHE) {
		__atomic_store_n(&mmap_cache[((uint32_t *)arg)[0]], ((uint64_t *)arg)[1], __ATOMIC_RELAXED);
	}
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

/*
 * The kernel speaks Panfrost 1.3, but Mesa then asks for the system
 * timestamp parameters, which it does not have: 1.1 (heap buffers,
 * allocated in full by the kernel) is all Mesa needs.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 1;
		version->name = strdup("panfrost");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup("Mali-G72 on Starbite XNU");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

/* Describe only: no buffer is made (size and layout for Mesa's import). */
int
starbite_drm_surface(int fd, struct starbite_drm_surface *surface)
{
	(void)fd;
	surface->flags = 1;
	int error = call(SBM_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
starbite_drm_import_host(int fd, void *addr, uint64_t size, uint32_t *handle)
{
	struct {
		uint64_t addr, size;
		uint32_t handle, pad;
		uint64_t va;
	} request = { .addr = (uint64_t)(uintptr_t)addr, .size = size };

	(void)fd;
	int error = call(SBM_IMPORT_HOST, &request, sizeof(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct starbite_drm_surface request = { .surface = (uint32_t)surface };

	(void)fd;
	int error = call(SBM_SURFACE, &request, sizeof(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	*handle = __atomic_add_fetch(&next_syncobj, 1, __ATOMIC_RELAXED);
	seq_new(*handle, (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? SEQ_SIGNALLED : SEQ_NONE);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	seq_forget(handle);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; seq_on && i < count; i++) {
		seq_set(handles[i], SEQ_NONE, 0);
	}
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; seq_on && i < count; i++) {
		seq_set(handles[i], SEQ_SIGNALLED, 0);
	}
	return 0;
}

/* timeout_nsec is an absolute CLOCK_MONOTONIC time: 0 (or past) = just look; the time left, relative */
static int64_t
relative_timeout(int64_t timeout_nsec)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
	return timeout_nsec < 0 ? INT64_MAX : timeout_nsec > now ? timeout_nsec - now : 0;
}

/* The wait without STARBITE_DRM_SEQ (and with it for what it does not follow): every chain queued so far. */
static int
wait_whole_queue(int64_t timeout_nsec, uint32_t *first_signaled)
{
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	if (async_on) {
		int64_t rel = relative_timeout(timeout_nsec);
		int error = call(SBM_WAIT_IDLE, &rel, sizeof(rel));
		if (error == ETIMEDOUT) {
			errno = ETIME;
			return -ETIME;
		}
	}
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	if (async_on && seq_on) {
		bool handled = false;
		int error = seq_wait(handles, num_handles, relative_timeout(timeout_nsec), flags, first_signaled, &handled);
		if (handled && error == ETIMEDOUT) {
			errno = ETIME;
			return -ETIME;
		}
		if (handled && error == 0) {
			return 0;
		}
		/* (not followed, or the kernel refused the request: as before) */
	}
	return wait_whole_queue(timeout_nsec, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)num_handles;
	(void)flags;
	/* (points are not followed: a point's fence may not be attached yet; the whole queue, with or without the switch) */
	return wait_whole_queue(timeout_nsec, first_signaled);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (seq_on) {
		pthread_mutex_lock(&seq_lock);
		struct seq_fence moved = seq_get_locked(src_handle);
		if (dst_point != 0 || src_point != 0) {
			moved = (struct seq_fence){ SEQ_UNKNOWN, 0 };   /* (timeline points: not followed) */
		}
		seq_set_locked(dst_handle, moved);
		pthread_mutex_unlock(&seq_lock);
	}
	return 0;
}

/* A signalled fence as a file: /dev/null, which poll() always finds ready. */
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	*sync_file_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (*sync_file_fd < 0) {
		return -1;
	}
	if (seq_on && *sync_file_fd < SEQ_FDS) {
		/* what it stands for, until it is imported (a later export that gets the same number replaces it) */
		pthread_mutex_lock(&seq_lock);
		seq_fds[*sync_file_fd] = seq_get_locked(handle);
		pthread_mutex_unlock(&seq_lock);
	}
	return 0;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	return drmSyncobjExportSyncFile(fd, handle, obj_fd);
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	if (seq_on) {
		struct seq_fence f = { SEQ_UNKNOWN, 0 };
		pthread_mutex_lock(&seq_lock);
		if (sync_file_fd >= 0 && sync_file_fd < SEQ_FDS && seq_fds[sync_file_fd].state != 0) {
			f = seq_fds[sync_file_fd];
			seq_fds[sync_file_fd].state = 0;        /* (once: a second import of it is not followed) */
		}
		seq_set_locked(handle, f);
		pthread_mutex_unlock(&seq_lock);
	}
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)obj_fd;
	int ret = drmSyncobjCreate(fd, 0, handle);
	seq_set(*handle, SEQ_UNKNOWN, 0);       /* (a syncobj from a file: not followed) */
	return ret;
}

/* ---- devices (PanVK) ---------------------------------------------------------------------------- */

static char starbite_node[] = "/dev/null";
static char *starbite_nodes[DRM_NODE_MAX] = { starbite_node, starbite_node, starbite_node };
static char starbite_compatible_0[] = "arm,mali-bifrost";
static char *starbite_compatible[] = { starbite_compatible_0, NULL };
static drmPlatformBusInfo starbite_bus = { "/starbite/mali" };
static drmPlatformDeviceInfo starbite_info = { starbite_compatible };

static drmDevicePtr
starbite_device(void)
{
	drmDevicePtr d = calloc(1, sizeof(*d));
	if (d != NULL) {
		d->nodes = starbite_nodes;
		d->available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
		d->bustype = DRM_BUS_PLATFORM;
		d->businfo.platform = &starbite_bus;
		d->deviceinfo.platform = &starbite_info;
	}
	return d;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (devices == NULL || max_devices <= 0) {
		return 1;
	}
	devices[0] = starbite_device();
	return devices[0] != NULL ? 1 : -ENOMEM;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	*device = starbite_device();
	return *device != NULL ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		free(*device);
		*device = NULL;
	}
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	(void)points;
	for (uint32_t i = 0; seq_on && i < count; i++) {
		seq_set(handles[i], SEQ_UNKNOWN, 0);    /* (timeline points: not followed) */
	}
	return 0;
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)handles;
	(void)flags;
	/*
	 * "Every point is reached" has to be true when it is said. With asynchronous submits it is once the GPU is
	 * done with everything submitted so far, so wait for that first (the kernel's queue: a snapshot of the
	 * last chain queued now, not of chains queued later). Without the wait a caller that reads a queue's timeline
	 * value right after another thread's submit takes chains that have not run yet for finished and reuses their
	 * memory under the GPU (seen as an MMU fault at address 0 and a GPU reset once the kernel really queued chains).
	 */
	if (async_on) {
		int64_t rel = INT64_MAX;
		(void)call(SBM_WAIT_IDLE, &rel, sizeof(rel));
	}
	/* every point is reached: report the furthest any timeline could be */
	for (uint32_t i = 0; i < count; i++) {
		points[i] = UINT64_MAX >> 1;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}
