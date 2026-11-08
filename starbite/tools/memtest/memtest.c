/*
 * memtest: a static init (no libSystem, no dyld, no shared cache) that checks
 * the VM primitives xnu-11417 gives a 16 KiB-page user map on the S9's 4 KiB-page
 * kernel. launchd dies from heap corruption in libSystem start-up (NOTES.md,
 * 2026-10-28), under iOS 16 and iPadOS 18 alike; this finds the broken primitive.
 *
 * Installed as /usr/appleinternal/sbin/launchd.memtest and selected with the
 * boot-arg launchdsuffix=memtest (DEVELOPMENT kernels allow static binaries).
 * Results go to /dev/console, which also feeds the persistent flash log, as
 * "MEMTEST <test>: PASS|FAIL ..." lines. A test that crashes shows as the last
 * "MEMTEST <test>: start" line. It exits at the end (critical exit: the kernel
 * prints its crash log and flushes the flash log before panicking).
 *
 * Build: tools/memtest/build.sh
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define SYS_exit        1
#define SYS_fork        2
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_wait4       7
#define SYS_munmap      73
#define SYS_mprotect    74
#define SYS_madvise     75
#define SYS_pread       153
#define SYS_mmap        197

#define TRAP_vm_allocate        (-10)
#define TRAP_vm_deallocate      (-12)
#define TRAP_task_self          (-28)

#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_SHARED      0x1
#define MAP_PRIVATE     0x2
#define MAP_ANON        0x1000
#define MADV_DONTNEED   4
#define MADV_FREE_REUSABLE 7
#define MADV_FREE_REUSE 8
#define O_RDONLY        0
#define O_WRONLY        1

#define K4      0x1000UL
#define K16     0x4000UL

static long last_err;
static int con = -1;

/* XNU arm64: number in x16, svc #0x80, carry set on error (x0 = errno). */
static long
sc(long n, long a, long b, long c, long d, long e, long f, long *x1out)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x16 __asm__("x16") = n;
	long nzcv;
	__asm__ volatile ("svc #0x80\n\tmrs %[nzcv], nzcv"
	    : "+r"(x0), "+r"(x1), [nzcv] "=r"(nzcv)
	    : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
	    : "memory", "cc", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17");
	if (x1out) {
		*x1out = x1;
	}
	/* Mach traps (negative numbers) return a kern_return_t, no carry. */
	last_err = (n > 0 && (nzcv & (1L << 29))) ? x0 : 0;
	return x0;
}

static u64
slen(const char *s)
{
	u64 n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
out(const char *s)
{
	if (con >= 0) {
		sc(SYS_write, con, (long)s, (long)slen(s), 0, 0, 0, 0);
	}
}

static void
outhex(u64 v)
{
	char b[19];
	b[0] = '0';
	b[1] = 'x';
	for (int i = 0; i < 16; i++) {
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
	}
	b[18] = 0;
	out(b);
}

static void
line(const char *test, const char *what, u64 a, u64 b)
{
	out("MEMTEST ");
	out(test);
	out(": ");
	out(what);
	out(" ");
	outhex(a);
	out(" ");
	outhex(b);
	out("\n");
}

static int fails;

static void
result(const char *test, int ok, u64 a, u64 b)
{
	line(test, ok ? "PASS" : "FAIL", a, b);
	fails += !ok;
}

/* Pattern word for an address: never 0, differs per 8 bytes and per seed. */
static u64
pat(u64 addr, u64 seed)
{
	return (addr * 0x9e3779b97f4a7c15UL) ^ seed ^ 0x5354415242495445UL;
}

static void
fill(u64 base, u64 len, u64 seed)
{
	for (u64 a = base; a < base + len; a += 8) {
		*(volatile u64 *)a = pat(a, seed);
	}
}

/* First bad address in [base, base+len) outside [skip0, skip1), or 0. */
static u64
check(u64 base, u64 len, u64 seed, u64 skip0, u64 skip1)
{
	for (u64 a = base; a < base + len; a += 8) {
		if (a >= skip0 && a < skip1) {
			continue;
		}
		if (*(volatile u64 *)a != pat(a, seed)) {
			return a;
		}
	}
	return 0;
}

static u64
anon(u64 len)
{
	long r = sc(SYS_mmap, 0, (long)len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0, 0);
	return last_err ? 0 : (u64)r;
}

static void
t_anon(void)
{
	out("MEMTEST anon: start\n");
	u64 len = 64 * K16;
	u64 b = anon(len);
	if (!b) {
		result("anon", 0, last_err, 0);
		return;
	}
	fill(b, len, 1);
	u64 bad = check(b, len, 1, 0, 0);
	result("anon", bad == 0, b, bad);
	sc(SYS_munmap, (long)b, (long)len, 0, 0, 0, 0, 0);
}

/* A 4 KiB sub-range of a 16 KiB page: what happens to its 12 KiB neighbours. */
static void
t_munmap4k(void)
{
	out("MEMTEST munmap4k: start\n");
	u64 len = 4 * K16;
	u64 b = anon(len);
	fill(b, len, 2);
	long r = sc(SYS_munmap, (long)(b + K16 + K4), (long)K4, 0, 0, 0, 0, 0);
	line("munmap4k", "ret/err", (u64)r, (u64)last_err);
	out("MEMTEST munmap4k: reading the other 12 KiB of that 16 KiB page\n");
	u64 bad = check(b, len, 2, b + K16 + K4, b + K16 + 2 * K4);
	result("munmap4k", bad == 0, b, bad);
}

static void
t_vmdealloc4k(void)
{
	out("MEMTEST vmdealloc4k: start\n");
	long task = sc(TRAP_task_self, 0, 0, 0, 0, 0, 0, 0);
	u64 addr = 0;
	long kr = sc(TRAP_vm_allocate, task, (long)&addr, (long)(4 * K16), 1 /* ANYWHERE */, 0, 0, 0);
	if (kr) {
		result("vmdealloc4k", 0, (u64)kr, 0);
		return;
	}
	fill(addr, 4 * K16, 3);
	kr = sc(TRAP_vm_deallocate, task, (long)(addr + K16 + K4), (long)K4, 0, 0, 0, 0);
	line("vmdealloc4k", "kr", (u64)kr, addr);
	out("MEMTEST vmdealloc4k: reading the other 12 KiB of that 16 KiB page\n");
	u64 bad = check(addr, 4 * K16, 3, addr + K16 + K4, addr + K16 + 2 * K4);
	result("vmdealloc4k", bad == 0, addr, bad);
}

/* libmalloc marks 4 KiB pieces reusable (launchd's last call before the abort). */
static void
t_reusable4k(void)
{
	out("MEMTEST reusable4k: start\n");
	u64 len = 4 * K16;
	u64 b = anon(len);
	fill(b, len, 4);
	long r = sc(SYS_madvise, (long)(b + K16 + K4), (long)K4, MADV_FREE_REUSABLE, 0, 0, 0, 0);
	line("reusable4k", "madvise reusable ret/err", (u64)r, (u64)last_err);
	/* Pressure: other allocations, so freed pages could be reused. */
	u64 other = anon(64 * K16);
	if (other) {
		fill(other, 64 * K16, 40);
	}
	r = sc(SYS_madvise, (long)(b + K16 + K4), (long)K4, MADV_FREE_REUSE, 0, 0, 0, 0);
	line("reusable4k", "madvise reuse ret/err", (u64)r, (u64)last_err);
	u64 bad = check(b, len, 4, b + K16 + K4, b + K16 + 2 * K4);
	result("reusable4k", bad == 0, b, bad);
	/* DONTNEED on 4 KiB: neighbours must stay. */
	fill(b, len, 5);
	r = sc(SYS_madvise, (long)(b + 2 * K16 + K4), (long)K4, MADV_DONTNEED, 0, 0, 0, 0);
	bad = check(b, len, 5, b + 2 * K16 + K4, b + 2 * K16 + 2 * K4);
	result("dontneed4k", bad == 0 && !last_err, (u64)last_err, bad);
}

static void
t_mprotect4k(void)
{
	out("MEMTEST mprotect4k: start\n");
	u64 len = 4 * K16;
	u64 b = anon(len);
	fill(b, len, 6);
	long r = sc(SYS_mprotect, (long)(b + K16 + K4), (long)K4, PROT_READ, 0, 0, 0, 0);
	line("mprotect4k", "ret/err", (u64)r, (u64)last_err);
	out("MEMTEST mprotect4k: writing the next 4 KiB of the same 16 KiB page\n");
	*(volatile u64 *)(b + K16 + 2 * K4) = pat(b + K16 + 2 * K4, 6);
	u64 bad = check(b, len, 6, 0, 0);
	result("mprotect4k", bad == 0, b, bad);
}

/* read() vs mmap() of the same file, and a MAP_PRIVATE write (COW). */
static void
t_file(const char *path, const char *name)
{
	out("MEMTEST file: start ");
	out(path);
	out("\n");
	long fd = sc(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0, 0);
	if (last_err) {
		result(name, 0, last_err, 0);
		return;
	}
	u64 len = 16 * K16;
	u64 buf = anon(len);
	long n = sc(SYS_pread, fd, (long)buf, (long)len, 0, 0, 0, 0);
	if (last_err || n <= 0) {
		result(name, 0, last_err, (u64)n);
		return;
	}
	u64 map = (u64)sc(SYS_mmap, 0, n, PROT_READ, MAP_SHARED, fd, 0, 0);
	if (last_err) {
		result(name, 0, last_err, 1);
		return;
	}
	u64 bad = ~0UL;
	for (long i = 0; i < n; i++) {
		if (((volatile u8 *)map)[i] != ((u8 *)buf)[i]) {
			bad = (u64)i;
			break;
		}
	}
	line(name, "size", (u64)n, map);
	result(name, bad == ~0UL, (u64)n, bad);
	sc(SYS_munmap, (long)map, n, 0, 0, 0, 0, 0);

	/* COW: private writable mapping, write the second 4 KiB, check the rest. */
	map = (u64)sc(SYS_mmap, 0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0, 0);
	if (!last_err && n > (long)(2 * K4)) {
		((volatile u8 *)map)[K4 + 1] ^= 0xff;
		bad = ~0UL;
		for (long i = 0; i < n; i++) {
			u8 want = ((u8 *)buf)[i] ^ (i == (long)(K4 + 1) ? 0xff : 0);
			if (((volatile u8 *)map)[i] != want) {
				bad = (u64)i;
				break;
			}
		}
		result("cow-private", bad == ~0UL, (u64)n, bad);
		long n2 = sc(SYS_pread, fd, (long)(buf + 8 * K16), K4 * 2, 0, 0, 0, 0);
		result("cow-file-unchanged", n2 > (long)(K4 + 1) &&
		    ((u8 *)buf)[8 * K16 + K4 + 1] == ((u8 *)buf)[K4 + 1], (u64)n2, 0);
		sc(SYS_munmap, (long)map, n, 0, 0, 0, 0, 0);
	}
	sc(SYS_close, fd, 0, 0, 0, 0, 0, 0);
	sc(SYS_munmap, (long)buf, (long)len, 0, 0, 0, 0, 0);
}

/* fork COW: the child scribbles over shared-before-fork memory; the parent's copy stays. */
static void
t_fork(void)
{
	out("MEMTEST fork: start\n");
	u64 len = 8 * K16;
	u64 b = anon(len);
	fill(b, len, 7);
	long x1 = 0;
	long pid = sc(SYS_fork, 0, 0, 0, 0, 0, 0, &x1);
	if (last_err) {
		result("fork", 0, last_err, 0);
		return;
	}
	if (x1) {               /* child */
		fill(b, len, 70);
		u64 bad = check(b, len, 70, 0, 0);
		sc(SYS_exit, bad ? 1 : 0, 0, 0, 0, 0, 0, 0);
	}
	int status = 0;
	sc(SYS_wait4, pid, (long)&status, 0, 0, 0, 0, 0);
	u64 bad = check(b, len, 7, 0, 0);
	result("fork-parent", bad == 0, (u64)status, bad);
}

/* malloc-like churn: many 16 KiB regions, free half, reallocate, check survivors. */
static void
t_churn(void)
{
	out("MEMTEST churn: start\n");
	long task = sc(TRAP_task_self, 0, 0, 0, 0, 0, 0, 0);
	static u64 r[256];
	for (int i = 0; i < 256; i++) {
		r[i] = 0;
		sc(TRAP_vm_allocate, task, (long)&r[i], (long)K16, 1, 0, 0, 0);
		fill(r[i], K16, 100 + i);
	}
	for (int i = 0; i < 256; i += 2) {
		sc(TRAP_vm_deallocate, task, (long)r[i], (long)K16, 0, 0, 0, 0);
	}
	for (int i = 0; i < 256; i += 2) {
		r[i] = 0;
		sc(TRAP_vm_allocate, task, (long)&r[i], (long)K4, 1, 0, 0, 0);
		fill(r[i], K4, 1000 + i);
	}
	u64 bad = 0;
	for (int i = 0; i < 256 && !bad; i++) {
		bad = check(r[i], (i & 1) ? K16 : K4, (i & 1) ? 100 + i : 1000 + i, 0, 0);
	}
	result("churn", bad == 0, 256, bad);
}

void
start_c(void)
{
	con = (int)sc(SYS_open, (long)"/dev/console", O_WRONLY, 0, 0, 0, 0, 0);
	if (last_err) {
		con = -1;
	}
	out("MEMTEST: static init, no libSystem; 16 KiB user map on a 4 KiB-page kernel\n");
	t_anon();
	t_file("/System/Library/CoreServices/SystemVersion.plist", "file-small");
	t_file("/System/Library/xpc/launchd.plist", "file-large");
	t_mprotect4k();
	t_vmdealloc4k();
	t_munmap4k();
	t_reusable4k();
	t_fork();
	t_churn();
	line("done", fails ? "FAILURES" : "ALL PASS", (u64)fails, 0);
	sc(SYS_exit, fails, 0, 0, 0, 0, 0, 0);
	for (;;) {
	}
}

/* LC_UNIXTHREAD entry: sp points at argc, only 8-byte aligned (dyld normally
 * realigns it); arm64 faults on an unaligned sp (EXC_ARM_SP_ALIGN, first run). */
__asm__(
	".globl __start\n"
	"__start:\n"
	"	mov x9, sp\n"
	"	and x9, x9, #~15\n"
	"	mov sp, x9\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	bl _start_c\n"
	"	b .\n"
	);
