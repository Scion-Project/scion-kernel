/*
 * SPDX-License-Identifier: MIT
 *
 * os_log of the program cam.dylib is in (cameracaptured, and Camera.app when the record names it), for the camera's trace
 * (cam_trace=1, camera_trace.c): logd is off on this root, so what CMCapture and AVFCapture say about a session (why it
 * does not run, what it was refused) is lost unless it is taken here. The decoding of os_log's argument buffer and the
 * in-place hook of libsystem_trace's four entry points are tools/ios27/metal/sbmtlc.c's (Scion's own, MIT), taken over as
 * they are; what is new is where the lines go:
 *   - every line to /private/var/local/oslog/<program>.log (the directory 30-overlay.sh makes; capped at 8 MiB), and
 *   - to the console (the kernel's log, which the phone test reads), as "CAMLOG[pid program] K: text", the errors and faults
 *     always and the other lines only when they name the camera's business (capture, camera, session, device, stream, ...),
 *     at most CONSOLE_MAX lines a process, so that a busy app cannot fill the console.
 * cam_trace=2 also turns os_log's info and debug levels on (os_log_type_enabled answers yes), which logd's absence otherwise
 * leaves off. Nothing here runs without cam_trace. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "camera_oslog.h"

#define FILE_MAX        (8 << 20)
#define CONSOLE_MAX     3000

static int file_fd = -1, console_fd = -1;
static off_t file_size;
static unsigned console_lines;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *program = "?";

/* ---- os_log's argument buffer: [summary][count] then per argument [descriptor][size][bytes]; type = descriptor >> 4 ---- */

enum { T_SCALAR = 0, T_COUNT = 1, T_STRING = 2, T_POINTER = 3, T_OBJECT = 4, T_WSTRING = 5, T_ERRNO = 6 };

struct arg {
	int type, size;
	const uint8_t *data;
};

static void put(char *out, size_t cap, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void
put(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len >= cap - 1) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(out + *len, cap - *len, fmt, ap);
	va_end(ap);
	if (n > 0) {
		*len = *len + (size_t)n < cap - 1 ? *len + (size_t)n : cap - 1;
	}
}

static uint64_t
uval(const struct arg *a)
{
	uint64_t v = 0;

	if (a->size == 1) {
		v = *(const uint8_t *)a->data;
	} else if (a->size == 2) {
		v = *(const uint16_t *)a->data;
	} else if (a->size == 4) {
		v = *(const uint32_t *)a->data;
	} else if (a->size >= 8) {
		memcpy(&v, a->data, 8);
	}
	return v;
}

static int64_t
sval(const struct arg *a)
{
	if (a->size == 1) {
		return *(const int8_t *)a->data;
	}
	if (a->size == 2) {
		return *(const int16_t *)a->data;
	}
	if (a->size == 4) {
		return *(const int32_t *)a->data;
	}
	return (int64_t)uval(a);
}

static void
decode(char *out, size_t cap, const char *fmt, const uint8_t *buf, uint32_t size)
{
	struct arg args[48];
	int nargs = 0, ai = 0;
	size_t len = 0;

	if (buf != NULL && size >= 2) {
		int count = buf[1];
		uint32_t off = 2;

		for (int i = 0; i < count && nargs < 48 && off + 2 <= size; i++) {
			int desc = buf[off], sz = buf[off + 1];

			if (off + 2 + (uint32_t)sz > size) {
				break;
			}
			args[nargs].type = desc >> 4;
			args[nargs].size = sz;
			args[nargs].data = buf + off + 2;
			nargs++;
			off += 2 + (uint32_t)sz;
		}
	}
	for (const char *p = fmt; p != NULL && *p && len < cap - 1; p++) {
		const struct arg *a;
		char c;

		if (*p != '%') {
			out[len++] = *p;
			continue;
		}
		p++;
		if (*p == '%') {
			out[len++] = '%';
			continue;
		}
		if (*p == '{') {
			while (*p && *p != '}') {
				p++;
			}
			if (*p) {
				p++;
			}
		}
		while (*p && strchr("-+ #0123456789.*'", *p)) {
			if (*p == '*' && ai < nargs && args[ai].type == T_COUNT) {
				ai++;
			}
			p++;
		}
		while (*p && strchr("hlqLjzt", *p)) {
			p++;
		}
		if ((c = *p) == '\0') {
			break;
		}
		while (ai < nargs && args[ai].type == T_COUNT) {
			ai++;
		}
		a = ai < nargs ? &args[ai++] : NULL;
		if (a == NULL) {
			put(out, cap, &len, "<?>");
			continue;
		}
		switch (c) {
		case '@': {
			void *obj = NULL;
			char tmp[600] = "?";

			if (a->size >= 8) {
				memcpy(&obj, a->data, 8);
			}
			if (obj == NULL) {
				put(out, cap, &len, "(null)");
				break;
			}
			if (CFGetTypeID((CFTypeRef)obj) == CFStringGetTypeID()) {
				CFStringGetCString((CFStringRef)obj, tmp, sizeof(tmp), kCFStringEncodingUTF8);
			} else {
				CFStringRef d = CFCopyDescription((CFTypeRef)obj);

				if (d != NULL) {
					CFStringGetCString(d, tmp, sizeof(tmp), kCFStringEncodingUTF8);
					CFRelease(d);
				}
			}
			put(out, cap, &len, "%s", tmp);
			break;
		}
		case 's': {
			const char *s = NULL;

			if (a->type == T_STRING && a->size >= 8) {
				memcpy(&s, a->data, 8);
			}
			put(out, cap, &len, "%s", s != NULL ? s : "(null)");
			break;
		}
		case 'd':
		case 'i':
			put(out, cap, &len, "%lld", (long long)sval(a));
			break;
		case 'u':
			put(out, cap, &len, "%llu", (unsigned long long)uval(a));
			break;
		case 'x':
		case 'X':
		case 'p':
			put(out, cap, &len, "0x%llx", (unsigned long long)uval(a));
			break;
		case 'f':
		case 'g':
		case 'e': {
			double d = 0;

			if (a->size == 8) {
				memcpy(&d, a->data, 8);
			} else if (a->size == 4) {
				float f;

				memcpy(&f, a->data, 4);
				d = f;
			}
			put(out, cap, &len, "%g", d);
			break;
		}
		case 'c':
			put(out, cap, &len, "%c", (char)uval(a));
			break;
		case 'm':
			put(out, cap, &len, "errno %lld", (long long)sval(a));
			break;
		case 'P':
			put(out, cap, &len, "<data %d bytes>", a->size);
			break;
		default:
			put(out, cap, &len, "<%%%c>", c);
			break;
		}
	}
	out[len] = '\0';
}

/* ---- where a line goes ------------------------------------------------------------------------------------------- */

static const char *const topics[] = {
	"captur", "camera", "session", "device", "stream", "vendor", "prewarm", "tcc", "interrupt", "graph", "source",
	"format", "preview", "fail", "error", "err=", "denied", "refus", "unavailable", "background", "foreground",
};

static bool
on_topic(const char *text)
{
	char low[400];
	size_t i;

	for (i = 0; i < sizeof(low) - 1 && text[i]; i++) {
		low[i] = (char)tolower((unsigned char)text[i]);
	}
	low[i] = '\0';
	for (i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
		if (strstr(low, topics[i]) != NULL) {
			return true;
		}
	}
	return false;
}

/* kind: 0 debug, 1 default/info, 2 error, 3 fault */
static void
capture(int kind, const char *fmt, const uint8_t *buf, uint32_t size)
{
	static const char *const kinds[] = { "D", "I", "E", "F" };
	char text[1500];
	struct timeval tv;

	decode(text, sizeof(text), fmt, buf, size);
	for (char *c = text; *c; c++) {
		*c = *c == '\n' ? ' ' : *c;
	}
	gettimeofday(&tv, NULL);
	pthread_mutex_lock(&lock);
	if (file_fd >= 0 && file_size < FILE_MAX) {
		int n = dprintf(file_fd, "%ld.%03d %s %s\n", (long)tv.tv_sec % 100000, (int)(tv.tv_usec / 1000), kinds[kind & 3], text);

		if (n > 0) {
			file_size += n;
		}
	}
	if (console_fd >= 0 && console_lines < CONSOLE_MAX && (kind >= 2 || on_topic(text))) {
		console_lines++;
		dprintf(console_fd, "CAMLOG[%d %s] %s: %.330s%s\n", getpid(), program, kinds[kind & 3], text,
		    console_lines == CONSOLE_MAX ? " (the last line to the console; the rest in /private/var/local/oslog)" : "");
	}
	pthread_mutex_unlock(&lock);
}

/* ---- libsystem_trace's entry points, patched in place (sbmtlc.c: dyld's __interpose misses calls inside the cache) ---- */

static __thread int in_hook;

static void
hook_impl(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
	(void)dso, (void)log;
	if (in_hook) {
		return;
	}
	in_hook = 1;
	capture(type == 0x10 ? 2 : type == 0x11 ? 3 : type == 2 ? 0 : 1, format, buf, size);
	in_hook = 0;
}

static uint32_t *
writable_entry(const char *name, vm_address_t *first, vm_size_t *len)
{
	uintptr_t at = (uintptr_t)dlsym(RTLD_DEFAULT, name) & 0x0000007fffffffffULL;
	vm_address_t last;

	if (at == 0) {
		return NULL;
	}
	*first = at & ~(vm_address_t)(vm_page_size - 1);
	last = (at + 15) & ~(vm_address_t)(vm_page_size - 1);
	*len = last - *first + vm_page_size;
	if (vm_protect(mach_task_self(), *first, *len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		return NULL;
	}
	return (uint32_t *)at;
}

static int
patch_entry(const char *name, void *hook)
{
	vm_address_t first;
	vm_size_t len;
	uintptr_t at = (uintptr_t)dlsym(RTLD_DEFAULT, name) & 0x0000007fffffffffULL;
	uint32_t *w;
	uint64_t target = (uint64_t)hook;

	if (at == 0 || (((uint32_t *)at)[0] != 0xd503237f && ((uint32_t *)at)[0] != 0xd503201f)) {
		return 0;       /* pacibsp / nop: the prologue measured in 24A446 */
	}
	if ((w = writable_entry(name, &first, &len)) == NULL) {
		return 0;
	}
	w[2] = (uint32_t)target;
	w[3] = (uint32_t)(target >> 32);
	w[1] = 0xd61f0200;      /* br x16 */
	w[0] = 0x58000050;      /* ldr x16, #8 */
	vm_protect(mach_task_self(), first, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(w, 16);
	return 1;
}

/* os_log_type_enabled() answers yes for every level (cam_trace=2): "mov w0, #1; ret" */
static int
all_levels_on(void)
{
	vm_address_t first;
	vm_size_t len;
	uint32_t *w = writable_entry("os_log_type_enabled", &first, &len);

	if (w == NULL) {
		return 0;
	}
	w[0] = 0x52800020;
	w[1] = 0xd65f03c0;
	vm_protect(mach_task_self(), first, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(w, 8);
	return 1;
}

int
scioncam_oslog_start(const char *name, int level)
{
	char path[256];
	struct stat st;
	int hooked;

	program = name;
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	snprintf(path, sizeof(path), "/private/var/local/oslog/%s.log", name);
	file_fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (file_fd >= 0 && fstat(file_fd, &st) == 0) {
		file_size = st.st_size;
	}
	if (file_fd >= 0) {
		dprintf(file_fd, "==== %s[%d] started, os_log captured here (cam_trace=%d) ====\n", name, getpid(), level);
	}
	hooked = patch_entry("_os_log_impl", (void *)hook_impl) + patch_entry("_os_log_error_impl", (void *)hook_impl) +
	    patch_entry("_os_log_fault_impl", (void *)hook_impl) + patch_entry("_os_log_debug_impl", (void *)hook_impl);
	if (level >= 2 && hooked > 0) {
		hooked += 10 * all_levels_on();
	}
	return hooked;
}
