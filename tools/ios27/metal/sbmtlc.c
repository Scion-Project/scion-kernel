/*
 * sbmtlc.dylib. Injected with sweetinject into Metal clients other than backboardd: any inserted
 * library makes the kernel start the program with ptrauth_disabled=1 (xnu 0324), so dyld takes the arm64 Metal plug-in
 * (StarbiteMetal.bundle, PanVK) into the arm64e process. That is all it needs to do.
 * Diagnostics: when /private/var/local/oslog-clients names this program (or "*"), os_log messages of this process
 * (_os_log_impl/_os_log_error_impl/_os_log_fault_impl/_os_log_debug_impl interposed; logd is off on this root) are
 * formatted here and appended to /private/var/local/oslog/<name>.log (capped at 8 MiB). Privacy flags are ignored
 * (the arguments are this process's own).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <os/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int log_fd = -1;
static off_t log_size;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static int listed(const char *path)
{
	char text[1024] = { 0 };
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	char *save = NULL;
	for (char *w = strtok_r(text, " ,\t\r\n", &save); w; w = strtok_r(NULL, " ,\t\r\n", &save))
		if (!strcmp(w, "*") || !strcmp(w, getprogname()))
			return 1;
	return 0;
}

void sbwall_start(void);
static void install_hooks(void);

__attribute__((constructor)) static void sbmtlc(void)
{
	sbwall_start();
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	int logging = listed("/private/var/local/oslog-clients");
	if (logging) {
		char path[256];
		mkdir("/private/var/local/oslog", 0777);
		snprintf(path, sizeof(path), "/private/var/local/oslog/%s.log", getprogname());
		log_fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
		struct stat st;
		if (log_fd >= 0 && fstat(log_fd, &st) == 0)
			log_size = st.st_size;
		if (log_fd >= 0)
			dprintf(log_fd, "==== %s[%d] started, os_log captured here ====\n", getprogname(), getpid());
		if (log_fd >= 0)
			install_hooks();
	}
	if (fd >= 0) {
		dprintf(fd, "SBMTLC[%d %s]: keys-off Metal client%s\n", getpid(), getprogname(), log_fd >= 0 ? ", os_log -> /private/var/local/oslog" : "");
		close(fd);
	}
}

/* os_log's argument buffer: [summary][count] then per argument [descriptor][size][bytes]; type = descriptor >> 4 */
enum { T_SCALAR = 0, T_COUNT = 1, T_STRING = 2, T_POINTER = 3, T_OBJECT = 4, T_WSTRING = 5, T_ERRNO = 6 };

struct arg {
	int type, size;
	const uint8_t *data;
};

static void put(char *out, size_t cap, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
	if (*len >= cap - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + *len, cap - *len, fmt, ap);
	va_end(ap);
	if (n > 0)
		*len = *len + (size_t)n < cap - 1 ? *len + (size_t)n : cap - 1;
}

static uint64_t uval(const struct arg *a)
{
	uint64_t v = 0;
	if (a->size == 1)
		v = *(const uint8_t *)a->data;
	else if (a->size == 2)
		v = *(const uint16_t *)a->data;
	else if (a->size == 4)
		v = *(const uint32_t *)a->data;
	else if (a->size >= 8)
		memcpy(&v, a->data, 8);
	return v;
}

static int64_t sval(const struct arg *a)
{
	if (a->size == 1)
		return *(const int8_t *)a->data;
	if (a->size == 2)
		return *(const int16_t *)a->data;
	if (a->size == 4)
		return *(const int32_t *)a->data;
	return (int64_t)uval(a);
}

static void format(char *out, size_t cap, const char *fmt, const uint8_t *buf, uint32_t size)
{
	struct arg args[48];
	int nargs = 0;
	if (buf && size >= 2) {
		int count = buf[1];
		uint32_t off = 2;
		for (int i = 0; i < count && nargs < 48 && off + 2 <= size; i++) {
			int desc = buf[off], sz = buf[off + 1];
			if (off + 2 + sz > size)
				break;
			args[nargs].type = desc >> 4;
			args[nargs].size = sz;
			args[nargs].data = buf + off + 2;
			nargs++;
			off += 2 + sz;
		}
	}
	size_t len = 0;
	int ai = 0;
	for (const char *p = fmt; p && *p && len < cap - 1; p++) {
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
			while (*p && *p != '}')
				p++;
			if (*p)
				p++;
		}
		char spec[32];
		size_t sl = 0;
		spec[sl++] = '%';
		while (*p && strchr("-+ #0123456789.*'", *p)) {
			if (*p == '*') {
				if (ai < nargs && args[ai].type == T_COUNT)
					ai++;
			} else if (sl < 20)
				spec[sl++] = *p;
			p++;
		}
		int lmod = 0;
		while (*p && strchr("hlqLjzt", *p)) {
			if (*p == 'l' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't')
				lmod++;
			p++;
		}
		char c = *p;
		if (!c)
			break;
		while (ai < nargs && args[ai].type == T_COUNT && c != '*')
			ai++;
		const struct arg *a = ai < nargs ? &args[ai++] : NULL;
		if (!a) {
			put(out, cap, &len, "<?>");
			continue;
		}
		switch (c) {
		case '@': {
			void *obj = NULL;
			if (a->size >= 8)
				memcpy(&obj, a->data, 8);
			if (!obj) {
				put(out, cap, &len, "(null)");
				break;
			}
			char tmp[1024] = "?";
			if (CFGetTypeID((CFTypeRef)obj) == CFStringGetTypeID())
				CFStringGetCString((CFStringRef)obj, tmp, sizeof(tmp), kCFStringEncodingUTF8);
			else {
				CFStringRef d = CFCopyDescription((CFTypeRef)obj);
				if (d) {
					CFStringGetCString(d, tmp, sizeof(tmp), kCFStringEncodingUTF8);
					CFRelease(d);
				}
			}
			put(out, cap, &len, "%s", tmp);
			break;
		}
		case 's': {
			const char *s = NULL;
			if (a->type == T_STRING && a->size >= 8)
				memcpy(&s, a->data, 8);
			put(out, cap, &len, "%s", s ? s : "(null)");
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
			if (a->size == 8)
				memcpy(&d, a->data, 8);
			else if (a->size == 4) {
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
		(void)lmod;
		(void)spec;
	}
	out[len] = 0;
}

static void capture(int kind, const char *fmt, uint8_t *buf, uint32_t size)
{
	if (log_fd < 0 || log_size > (8 << 20))
		return;
	char text[3000];
	format(text, sizeof(text), fmt, buf, size);
	struct timeval tv;
	gettimeofday(&tv, NULL);
	static const char *const kinds[] = { "D", "I", "E", "F" };
	pthread_mutex_lock(&log_lock);
	int n = dprintf(log_fd, "%ld.%03d %s %s\n", (long)tv.tv_sec % 100000, (int)(tv.tv_usec / 1000), kinds[kind & 3], text);
	if (n > 0)
		log_size += n;
	pthread_mutex_unlock(&log_lock);
}

/*
 * dyld's __interpose does not reach calls made inside the shared cache (boot B+C: PosterBoard.log held only its
 * header), so the four libsystem_trace entry points are patched in place: their first 16 bytes become
 * "ldr x16, #8; br x16; .quad hook". The hooks record and return (logd is off on this root; nothing is lost).
 */
static __thread int in_hook;
static void hook_impl(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
	(void)dso; (void)log;
	if (in_hook)
		return;
	in_hook = 1;
	capture(type == 0x10 ? 2 : type == 0x11 ? 3 : type == 2 ? 0 : 1, format, buf, size);
	in_hook = 0;
}

#include <dlfcn.h>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>

static int patch_entry(const char *name, void *hook)
{
	uintptr_t at = (uintptr_t)dlsym(RTLD_DEFAULT, name) & 0x0000007fffffffffULL;
	if (!at)
		return 0;
	uint32_t *w = (uint32_t *)at;
	if (w[0] != 0xd503237f && w[0] != 0xd503201f) /* pacibsp / nop: the prologue measured in 24A446 */
		return 0;
	vm_address_t first = at & ~(vm_address_t)(vm_page_size - 1), last = (at + 15) & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t len = last - first + vm_page_size;
	if (vm_protect(mach_task_self(), first, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
		return 0;
	uint64_t target = (uint64_t)hook;
	w[2] = (uint32_t)target;
	w[3] = (uint32_t)(target >> 32);
	w[1] = 0xd61f0200; /* br x16 */
	w[0] = 0x58000050; /* ldr x16, #8 */
	vm_protect(mach_task_self(), first, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(w, 16);
	return 1;
}

static void install_hooks(void)
{
	int n = patch_entry("_os_log_impl", (void *)hook_impl) + patch_entry("_os_log_error_impl", (void *)hook_impl) +
		patch_entry("_os_log_fault_impl", (void *)hook_impl) + patch_entry("_os_log_debug_impl", (void *)hook_impl);
	if (log_fd >= 0)
		dprintf(log_fd, "==== %d of 4 os_log entry points hooked ====\n", n);
}
