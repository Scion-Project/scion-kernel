// dptest.m - private on-device data-protection / keystore probe (ios27-fable-keystore). Read-mostly.
// Parent re-execs itself once per section (fork-safe for XPC/ObjC): dptest [mobile] [nosleep]
// child: dptest --sec N [IDX] LOGPATH SCRATCH
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>

extern char **environ;
#define F_GETPROTECTIONCLASS_ 63
#define F_SETPROTECTIONCLASS_ 64
#define DPATTR 0x40000000u

static int LF = -1;
static void L(const char *fmt, ...) {
	char b[4096]; va_list ap; va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
	if (n < 0) return; if (n > (int)sizeof b - 2) n = sizeof b - 2;
	b[n++] = '\n';
	if (LF >= 0) { write(LF, b, n); fsync(LF); }
	write(2, b, n);
}
static void LS(const char *label, NSString *s) { L("%s: %s", label, s ? [s UTF8String] : "(nil)"); }
static void LO(const char *label, id o) { LS(label, o ? [NSString stringWithFormat:@"%@", o] : nil); }
static void LC(const char *label, CFTypeRef o) {
	if (!o) { L("%s: (null)", label); return; }
	CFStringRef d = CFCopyDescription(o);
	char *buf = malloc(65536); buf[0] = 0;
	if (d) { CFStringGetCString(d, buf, 65536, kCFStringEncodingUTF8); CFRelease(d); }
	L("%s: %s", label, buf); free(buf);
}
static void hexdump(const unsigned char *p, size_t n) {
	if (n > 256) n = 256;
	for (size_t i = 0; i < n; i += 16) {
		char line[128]; int o = snprintf(line, sizeof line, "    %04zx:", i);
		for (size_t j = i; j < i + 16 && j < n; j++) o += snprintf(line + o, sizeof line - o, " %02x", p[j]);
		L("%s", line);
	}
}
#define ERR(e) (e), strerror(e)

static const char *LOGP, *SCR;

// ---------- header ----------
static void header(int mobile) {
	struct timeval tv; gettimeofday(&tv, 0); char tb[64]; struct tm tm; time_t t = tv.tv_sec; gmtime_r(&t, &tm);
	strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%SZ", &tm);
	struct utsname u; uname(&u);
	char kv[512] = "?", ba[1024] = "?", bt[64] = "?"; size_t l = sizeof kv; sysctlbyname("kern.version", kv, &l, 0, 0);
	l = sizeof ba; if (sysctlbyname("kern.bootargs", ba, &l, 0, 0)) snprintf(ba, sizeof ba, "(err %d %s)", ERR(errno));
	struct timeval bts; l = sizeof bts; if (!sysctlbyname("kern.boottime", &bts, &l, 0, 0)) snprintf(bt, sizeof bt, "%ld", (long)bts.tv_sec);
	L("");
	L("==================== dptest start %s (unix %ld) mode=%s uid=%d euid=%d pid=%d ====================", tb, (long)tv.tv_sec, mobile ? "mobile" : "root", getuid(), geteuid(), getpid());
	L("uname: %s %s %s %s", u.sysname, u.release, u.version, u.machine);
	L("kern.version: %s", kv);
	L("kern.bootargs: %s", ba);
	L("kern.boottime: %s", bt);
}

// ---------- section 1 ----------
static void sec1(void) {
	const char *ps[] = {"/private/var", "/"};
	for (int i = 0; i < 2; i++) {
		struct statfs s; memset(&s, 0, sizeof s);
		if (statfs(ps[i], &s)) { L("statfs(%s) FAILED errno=%d %s", ps[i], ERR(errno)); continue; }
		L("statfs(%s): fstypename=%s mntonname=%s mntfromname=%s f_flags=0x%x MNT_CPROTECT(0x80)=%s bsize=%u blocks=%llu bfree=%llu",
		  ps[i], s.f_fstypename, s.f_mntonname, s.f_mntfromname, s.f_flags, (s.f_flags & 0x80) ? "SET" : "clear", s.f_bsize,
		  (unsigned long long)s.f_blocks, (unsigned long long)s.f_bfree);
	}
}

// ---------- section 2 helpers ----------
typedef struct { u_int32_t len; u_int32_t val; } __attribute__((packed)) dpbuf_t;
static int attr_get(int fd, const char *path, long long *out) {
	struct attrlist al; memset(&al, 0, sizeof al); al.bitmapcount = ATTR_BIT_MAP_COUNT; al.commonattr = DPATTR;
	dpbuf_t b; memset(&b, 0, sizeof b);
	int r = fd >= 0 ? fgetattrlist(fd, &al, &b, sizeof b, 0) : getattrlist(path, &al, &b, sizeof b, 0);
	if (r) return errno ? errno : -1;
	*out = b.val; return 0;
}
static const char *dstr(long long v) {
	static const char *n[] = {"?0(NONE/none-flag)", "A(Complete)", "B(CompleteUnlessOpen)", "C(AfterFirstUnlock)", "D(None)", "E(CompleteUnlessOpen-alt)"};
	return (v >= 0 && v <= 5) ? n[v] : "?";
}
static void rep(const char *tag, int fd, const char *path) {
	int c = fcntl(fd, F_GETPROTECTIONCLASS_); int e1 = errno;
	long long a = -1; int ae = attr_get(fd, NULL, &a);
	char s1[96], s2[96];
	if (c < 0) snprintf(s1, sizeof s1, "FAIL errno=%d %s", ERR(e1)); else snprintf(s1, sizeof s1, "%d %s", c, dstr(c));
	if (ae) snprintf(s2, sizeof s2, "FAIL errno=%d %s", ERR(ae)); else snprintf(s2, sizeof s2, "%lld (0x%llx)", a, a);
	L("  [%s] %s: fcntl(GETPROTECTIONCLASS)=%s ; fgetattrlist(DATA_PROTECT_FLAGS)=%s", tag, path, s1, s2);
}
static void rep_path(const char *tag, const char *path) {
	long long a = -1; int ae = attr_get(-1, path, &a);
	int fd = open(path, O_RDONLY | O_NONBLOCK); int oe = errno;
	char s1[96] = "", s2[96];
	if (fd < 0) snprintf(s1, sizeof s1, "open FAIL errno=%d %s", ERR(oe));
	else { int c = fcntl(fd, F_GETPROTECTIONCLASS_); int e1 = errno; if (c < 0) snprintf(s1, sizeof s1, "FAIL errno=%d %s", ERR(e1)); else snprintf(s1, sizeof s1, "%d %s", c, dstr(c)); close(fd); }
	if (ae) snprintf(s2, sizeof s2, "FAIL errno=%d %s", ERR(ae)); else snprintf(s2, sizeof s2, "%lld (0x%llx)", a, a);
	struct stat st; char sts[96] = "stat FAIL"; if (!stat(path, &st)) snprintf(sts, sizeof sts, "mode=%o uid=%d gid=%d%s", st.st_mode & 07777, st.st_uid, st.st_gid, S_ISDIR(st.st_mode) ? " DIR" : "");
	L("  [%s] %s: fcntl=%s ; getattrlist=%s ; %s", tag, path, s1, s2, sts);
}
static void listdir_cls(const char *dir, int cls) {
	DIR *d = opendir(dir); if (!d) { L("  opendir(%s) FAIL errno=%d %s", dir, ERR(errno)); return; }
	struct dirent *e; int n = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (++n > 200) { L("  ... (truncated at 200)"); break; }
		char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (cls) rep_path("ls", p); else L("  entry: %s (d_type=%d)", p, e->d_type);
	}
	closedir(d); L("  (%d entries in %s)", n, dir);
}
static void setcls(int fd, int cls, const char *what) {
	int r = fcntl(fd, F_SETPROTECTIONCLASS_, cls); int e = errno;
	if (r) L("  SET class %d on %s: FAIL r=%d errno=%d %s", cls, what, r, ERR(e)); else L("  SET class %d on %s: ok r=%d", cls, what, r);
}

static void sec2(void) {
	char dir[512]; snprintf(dir, sizeof dir, "%s", SCR);
	char cmd[600]; snprintf(cmd, sizeof cmd, "%s", dir);
	// mkdir -p
	for (char *p = cmd + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(cmd, 0755); *p = '/'; }
	if (mkdir(dir, 0755) && errno != EEXIST) L("mkdir(%s) FAIL errno=%d %s", dir, ERR(errno));
	L("scratch dir: %s", dir);
	rep_path("scratchdir", dir);
	char f1[600]; snprintf(f1, sizeof f1, "%s/plain.%d", dir, getpid()); unlink(f1);
	int fd = open(f1, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { L("open(%s, O_CREAT) FAIL errno=%d %s", f1, ERR(errno)); }
	else {
		write(fd, "dptest\n", 7); fsync(fd);
		L("-- 2a plain open() file %s", f1);
		rep("fresh", fd, f1);
		for (int c = 1; c <= 4; c++) {
			char w[32]; snprintf(w, sizeof w, "class%d", c);
			setcls(fd, c, f1); rep(w, fd, f1);
		}
		// set class 3 then persistence test per class: close/reopen after setting each
		for (int c = 1; c <= 4; c++) {
			setcls(fd, c, f1); close(fd);
			fd = open(f1, O_RDWR); if (fd < 0) { L("  reopen FAIL errno=%d %s", ERR(errno)); break; }
			char w[32]; snprintf(w, sizeof w, "reopen-after-%d", c); rep(w, fd, f1);
		}
		if (fd >= 0) close(fd);
		// by-path getattrlist
		rep_path("bypath", f1);
		// setattrlist by path
		{
			struct attrlist al; memset(&al, 0, sizeof al); al.bitmapcount = ATTR_BIT_MAP_COUNT; al.commonattr = DPATTR;
			for (u_int32_t v = 1; v <= 4; v++) {
				u_int32_t val = v;
				int r = setattrlist(f1, &al, &val, sizeof val, 0);
				L("  setattrlist(DATA_PROTECT_FLAGS=%u) by path: r=%d errno=%d %s", v, r, r ? errno : 0, r ? strerror(errno) : "ok");
				rep_path("after-setattrlist", f1);
			}
		}
	}
	// open_dprotected_np
	L("-- 2b open_dprotected_np(path, O_CREAT|O_RDWR, 3, 0, 0644)");
	typedef int (*odp_t)(const char *, int, int, int, ...);
	odp_t odp = (odp_t)dlsym(RTLD_DEFAULT, "open_dprotected_np");
	char f2[600]; snprintf(f2, sizeof f2, "%s/dp3.%d", dir, getpid()); unlink(f2);
	if (!odp) L("  open_dprotected_np: dlsym NULL");
	else {
		int g = odp(f2, O_CREAT | O_RDWR, 3, 0, 0644); int e = errno;
		if (g < 0) L("  open_dprotected_np FAIL errno=%d %s", ERR(e));
		else { L("  open_dprotected_np ok fd=%d", g); rep("dp3", g, f2); close(g); rep_path("dp3-bypath", f2); }
	}
	// directory
	L("-- 2c directory class + inheritance");
	char d2[600]; snprintf(d2, sizeof d2, "%s/sub.%d", dir, getpid()); mkdir(d2, 0755);
	int dfd = open(d2, O_RDONLY);
	if (dfd < 0) L("  open(dir) FAIL errno=%d %s", ERR(errno));
	else {
		rep("dir-fresh", dfd, d2); setcls(dfd, 3, d2); rep("dir-after3", dfd, d2); close(dfd);
		char f3[700]; snprintf(f3, sizeof f3, "%s/child", d2);
		int h = open(f3, O_CREAT | O_RDWR, 0644);
		if (h < 0) L("  create in dir FAIL errno=%d %s", ERR(errno)); else { rep("inherit-child", h, f3); close(h); }
		rep_path("inherit-child-bypath", f3);
	}
	// NSFileManager / NSURL
	L("-- 2d NSFileManager / NSURL");
	@autoreleasepool {
		NSFileManager *fm = [NSFileManager defaultManager];
		NSString *p = [NSString stringWithFormat:@"%s/ns.%d", dir, getpid()];
		[fm removeItemAtPath:p error:nil];
		BOOL ok = [fm createFileAtPath:p contents:[@"x" dataUsingEncoding:NSUTF8StringEncoding] attributes:nil];
		L("  createFileAtPath ok=%d", ok);
		NSError *er = nil;
		NSDictionary *at = [fm attributesOfItemAtPath:p error:&er];
		LO("  attributesOfItemAtPath NSFileProtectionKey", at[NSFileProtectionKey]); LO("  err", er);
		NSURL *u = [NSURL fileURLWithPath:p]; id v = nil; er = nil;
		[u getResourceValue:&v forKey:NSURLFileProtectionKey error:&er];
		LO("  NSURLFileProtectionKey", v); LO("  err", er);
		NSArray *prots = @[NSFileProtectionNone, NSFileProtectionComplete];
		NSArray *pn = @[@"None", @"Complete"];
		for (int i = 0; i < 2; i++) {
			er = nil; BOOL r = [fm setAttributes:@{NSFileProtectionKey : prots[i]} ofItemAtPath:p error:&er];
			L("  setAttributes NSFileProtection%s: ret=%d", [pn[i] UTF8String], r); LO("  err", er);
			er = nil; at = [fm attributesOfItemAtPath:p error:&er];
			LO("  readback NSFileProtectionKey", at[NSFileProtectionKey]);
			v = nil; [[NSURL fileURLWithPath:p] getResourceValue:&v forKey:NSURLFileProtectionKey error:&er]; LO("  readback NSURLFileProtectionKey", v);
			rep_path("ns-after", [p UTF8String]);
		}
	}
	// existing paths
	L("-- 2e existing paths (read only)");
	const char *ex[] = {"/private/var/mobile/Library/Preferences", "/private/var/mobile/Library/Keychains", "/private/var/keybags",
		"/private/var/mobile/Containers", "/private/var/containers", "/private/var/db/lsd", "/private/var/mobile/Library/Caches", "/usr/libexec/keybagd"};
	for (unsigned i = 0; i < sizeof ex / sizeof *ex; i++) rep_path("exist", ex[i]);
	L("-- Keychains dir files:"); listdir_cls("/private/var/mobile/Library/Keychains", 1);
	L("-- /private/var/keybags files:"); listdir_cls("/private/var/keybags", 1);
	L("-- /private/var/db/lsd listing:"); listdir_cls("/private/var/db/lsd", 0);
}

// ---------- section 3 ----------
static void sec3(void) {
	void *h = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_NOW);
	if (!h) { L("dlopen MobileKeyBag FAIL: %s", dlerror()); return; }
	L("dlopen MobileKeyBag ok");
	typedef int (*i_d)(CFDictionaryRef); typedef int (*i_v)(void); typedef CFDictionaryRef (*d_d)(CFDictionaryRef);
	i_d gls = (i_d)dlsym(h, "MKBGetDeviceLockState"); i_v uns = (i_v)dlsym(h, "MKBDeviceUnlockedSinceBoot");
	d_d gli = (d_d)dlsym(h, "MKBGetDeviceLockStateInfo"); i_v fmt = (i_v)dlsym(h, "MKBDeviceFormattedForContentProtection");
	d_d gcf = (d_d)dlsym(h, "MKBGetDeviceConfigurations");
	L("symbols: GetDeviceLockState=%p UnlockedSinceBoot=%p LockStateInfo=%p Formatted=%p GetDeviceConfigurations=%p", gls, uns, gli, fmt, gcf);
	if (gls) { errno = 0; int r = gls(NULL); L("MKBGetDeviceLockState(NULL) = %d (0x%x)", r, r); }
	if (uns) { int r = uns(); L("MKBDeviceUnlockedSinceBoot() = %d (0x%x)", r, r); }
	if (gli) { CFDictionaryRef d = gli(NULL); LC("MKBGetDeviceLockStateInfo(NULL)", d); }
	if (fmt) { int r = fmt(); L("MKBDeviceFormattedForContentProtection() = %d (0x%x)", r, r); }
	if (gcf) { CFDictionaryRef d = gcf(NULL); LC("MKBGetDeviceConfigurations(NULL)", d); } else L("MKBGetDeviceConfigurations: symbol missing, skipped");
}

// ---------- section 4 ----------
typedef struct { int sel; int nin; uint64_t in[2]; int nout; size_t sout; const char *desc; } call_t;
static const call_t CALLS[] = {
	{7, 1, {0, 0}, 1, 0, "sel7 in=0"},
	{7, 1, {(uint64_t)-3, 0}, 1, 0, "sel7 in=-3"},
	{14, 0, {0, 0}, 1, 0, "sel14"},
	{17, 1, {0, 0}, 0, 4096, "sel17 in=0 sout=4096"},
	{35, 1, {0, 0}, 0, 4096, "sel35 in=0 sout=4096"},
	{31, 1, {0, 0}, 0, 32768, "sel31 in=0 sout=32768"},
	{23, 1, {0, 0}, 0, 16, "sel23 in=0 sout=16"},
	{40, 2, {0, 0}, 1, 0, "sel40 in=0,0"},
	{107, 0, {0, 0}, 1, 0, "sel107"},
	{103, 0, {0, 0}, 1, 0, "sel103"},
};
#define NCALLS ((int)(sizeof CALLS / sizeof *CALLS))
static void sec4(int idx) {
	io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("AppleKeyStore"));
	if (idx == 0) L("IOServiceGetMatchingService(AppleKeyStore) = 0x%x", svc);
	if (!svc) { L("[%d] no AppleKeyStore service", idx); return; }
	io_connect_t c = 0; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
	L("[%d] IOServiceOpen(type 0) kr=0x%x (%s) conn=0x%x", idx, kr, mach_error_string(kr), c);
	if (kr) return;
	const call_t *k = &CALLS[idx];
	uint64_t out[4] = {0}; uint32_t nout = k->nout;
	void *so = k->sout ? calloc(1, k->sout) : NULL; size_t sol = k->sout;
	kr = IOConnectCallMethod(c, k->sel, k->nin ? k->in : NULL, k->nin, NULL, 0, k->nout ? out : NULL, k->nout ? &nout : NULL, so, so ? &sol : NULL);
	L("[%d] %s: kr=0x%x (%s) nScalarOut=%u scalar0=0x%llx (%lld) structOutSize=%zu", idx, k->desc, kr, mach_error_string(kr), k->nout ? nout : 0,
	  (unsigned long long)out[0], (long long)out[0], so ? sol : 0);
	if (so && sol) hexdump(so, sol > k->sout ? k->sout : sol);
	IOServiceClose(c); IOObjectRelease(svc);
}

// ---------- section 5 ----------
static void kc_one(const char *acct, CFTypeRef acc, int idx) {
	CFStringRef a = CFStringCreateWithCString(0, acct, kCFStringEncodingUTF8);
	unsigned char rnd[16]; FILE *f = fopen("/dev/urandom", "r"); if (f) { fread(rnd, 1, 16, f); fclose(f); } else for (int i = 0; i < 16; i++) rnd[i] = rand();
	CFDataRef data = CFDataCreate(0, rnd, 16);
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword); CFDictionarySetValue(q, kSecAttrService, CFSTR("dptest")); CFDictionarySetValue(q, kSecAttrAccount, a);
	SecItemDelete(q);
	CFMutableDictionaryRef add = CFDictionaryCreateMutableCopy(0, 0, q);
	CFDictionarySetValue(add, kSecValueData, data); CFDictionarySetValue(add, kSecAttrAccessible, acc);
	OSStatus s = SecItemAdd(add, NULL);
	CFStringRef m = SecCopyErrorMessageString(s, NULL); char mb[256] = ""; if (m) { CFStringGetCString(m, mb, sizeof mb, kCFStringEncodingUTF8); CFRelease(m); }
	LC("  accessible-constant", acc);
	L("  [%s] SecItemAdd -> %d (%s)", acct, (int)s, mb);
	CFMutableDictionaryRef g = CFDictionaryCreateMutableCopy(0, 0, q);
	CFDictionarySetValue(g, kSecReturnData, kCFBooleanTrue); CFDictionarySetValue(g, kSecMatchLimit, kSecMatchLimitOne);
	CFTypeRef out = NULL; s = SecItemCopyMatching(g, &out);
	m = SecCopyErrorMessageString(s, NULL); mb[0] = 0; if (m) { CFStringGetCString(m, mb, sizeof mb, kCFStringEncodingUTF8); CFRelease(m); }
	L("  [%s] SecItemCopyMatching -> %d (%s) dataMatches=%s", acct, (int)s, mb, (out && CFGetTypeID(out) == CFDataGetTypeID() && CFEqual(out, data)) ? "YES" : "no");
	if (out) CFRelease(out);
	CFMutableDictionaryRef g2 = CFDictionaryCreateMutableCopy(0, 0, q);
	CFDictionarySetValue(g2, kSecReturnAttributes, kCFBooleanTrue); CFTypeRef o2 = NULL; s = SecItemCopyMatching(g2, &o2);
	if (!s) LC("  attrs", o2); if (o2) CFRelease(o2);
	(void)idx;
}
static void sec5(void) {
	L("kSecAttrAccessibleAfterFirstUnlock"); kc_one("a1", kSecAttrAccessibleAfterFirstUnlock, 0);
	L("kSecAttrAccessibleWhenUnlocked"); kc_one("a2", kSecAttrAccessibleWhenUnlocked, 1);
	L("kSecAttrAccessibleAlwaysThisDeviceOnly"); kc_one("a3", kSecAttrAccessibleAlwaysThisDeviceOnly, 2);
	const char *ac[] = {"a1", "a2", "a3"};
	for (int i = 0; i < 3; i++) {
		CFMutableDictionaryRef q = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword); CFDictionarySetValue(q, kSecAttrService, CFSTR("dptest"));
		CFStringRef a = CFStringCreateWithCString(0, ac[i], kCFStringEncodingUTF8); CFDictionarySetValue(q, kSecAttrAccount, a);
		OSStatus s = SecItemDelete(q); L("  [%s] SecItemDelete -> %d", ac[i], (int)s);
	}
}

// ---------- section 6 ----------
static void sec6(void) {
	void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	if (!h) { L("dlopen libMobileGestalt FAIL: %s", dlerror()); return; }
	typedef CFTypeRef (*mg_t)(CFStringRef, CFDictionaryRef);
	mg_t mg = (mg_t)dlsym(h, "MGCopyAnswer"); if (!mg) { L("MGCopyAnswer: dlsym NULL"); return; }
	const char *ks[] = {"PasswordConfigured", "PasswordProtected", "HasSEP", "ProductType", "HWModelStr", "BuildVersion", "UniqueDeviceID", "SerialNumber", "DeviceClass", "ActivationState"};
	for (unsigned i = 0; i < sizeof ks / sizeof *ks; i++) {
		CFStringRef k = CFStringCreateWithCString(0, ks[i], kCFStringEncodingUTF8);
		CFTypeRef r = mg(k, NULL); char lab[128]; snprintf(lab, sizeof lab, "MGCopyAnswer(%s)", ks[i]); LC(lab, r);
	}
}

// ---------- section 7 ----------
static void count_dir(const char *p) {
	struct stat st; if (stat(p, &st)) { L("  stat(%s) FAIL errno=%d %s", p, ERR(errno)); return; }
	L("  stat(%s): mode=%o uid=%d gid=%d", p, st.st_mode & 07777, st.st_uid, st.st_gid);
	DIR *d = opendir(p); if (!d) { L("  opendir FAIL errno=%d %s", ERR(errno)); return; }
	int n = 0; struct dirent *e; while ((e = readdir(d))) { if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++; } closedir(d);
	L("  entries in %s: %d", p, n);
}
static void sec7(void) {
	void *h = dlopen("/usr/lib/system/libsystem_containermanager.dylib", RTLD_NOW);
	L("dlopen libsystem_containermanager: %s", h ? "ok" : dlerror());
	typedef char *(*cf_t)(void *, const char *, void *);
	cf_t f = (cf_t)dlsym(RTLD_DEFAULT, "container_system_group_path_for_identifier");
	if (!f) L("container_system_group_path_for_identifier: dlsym NULL");
	else {
		unsigned char err[256]; memset(err, 0, sizeof err);   // real 3rd arg is a container_error_t*, oversized buffer on purpose (guess)
		char *p = f(NULL, "systemgroup.com.apple.configurationprofiles", err);
		L("container_system_group_path_for_identifier(configurationprofiles) = %s ; errbuf first8=%02x%02x%02x%02x%02x%02x%02x%02x", p ? p : "NULL",
		  err[0], err[1], err[2], err[3], err[4], err[5], err[6], err[7]);
	}
	count_dir("/private/var/containers/Shared/SystemGroup");
	count_dir("/private/var/mobile/Containers/Data/Application");
}

// ---------- driver ----------
static void child(int argc, char **argv) {
	int sec = atoi(argv[2]); int ai = 3; int idx = 0;
	if (sec == 4) idx = atoi(argv[ai++]);
	LOGP = argv[ai++]; SCR = argv[ai++];
	LF = open(LOGP, O_WRONLY | O_APPEND | O_CREAT, 0666);
	alarm(20);
	@autoreleasepool {
		switch (sec) {
		case 1: sec1(); break; case 2: sec2(); break; case 3: sec3(); break; case 4: sec4(idx); break;
		case 5: sec5(); break; case 6: sec6(); break; case 7: sec7(); break;
		}
	}
	_exit(0);
}
static int run_child(const char *self, int sec, int idx, int mobile) {
	char ss[8], is[8]; snprintf(ss, sizeof ss, "%d", sec); snprintf(is, sizeof is, "%d", idx);
	char *av[10]; int n = 0; av[n++] = (char *)self; av[n++] = "--sec"; av[n++] = ss; if (sec == 4) av[n++] = is;
	av[n++] = (char *)LOGP; av[n++] = (char *)SCR; av[n] = NULL;
	pid_t pid; int r = posix_spawn(&pid, self, NULL, NULL, av, environ);
	if (r) { L("  posix_spawn FAIL %d %s", ERR(r)); return -1; }
	int st = 0; for (int i = 0; i < 300; i++) { pid_t w = waitpid(pid, &st, WNOHANG); if (w == pid) goto done; usleep(100000); }
	kill(pid, SIGKILL); waitpid(pid, &st, 0); L("  !! section %d killed by parent after 30s timeout", sec); return -2;
done:
	if (WIFSIGNALED(st)) L("  !! section %d (idx %d) died with signal %d (%s)", sec, idx, WTERMSIG(st), strsignal(WTERMSIG(st)));
	else L("  (section %d idx %d exit status %d)", sec, idx, WEXITSTATUS(st));
	(void)mobile; return 0;
}
int main(int argc, char **argv) {
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc > 2 && !strcmp(argv[1], "--sec")) child(argc, argv);
	int mobile = 0, nosleep = 0;
	for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "mobile")) mobile = 1; if (!strcmp(argv[i], "nosleep")) nosleep = 1; }
	LOGP = mobile ? "/private/var/local/oslog/dptest-mobile.log" : "/private/var/local/oslog/dptest.log";
	SCR = mobile ? "/private/var/mobile/Library/dptest" : "/private/var/mobile/Library/dptest-root";
	LF = open(LOGP, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (LF < 0) { fprintf(stderr, "cannot open log %s: %s\n", LOGP, strerror(errno)); return 1; }
	fchmod(LF, 0666);
	header(mobile);
	char self[1024]; uint32_t sl = sizeof self;
	if (_NSGetExecutablePath(self, &sl)) strcpy(self, "/usr/local/bin/dptest");
	if (!nosleep) { L("sleeping 60 s (SpringBoard warm-up)"); sleep(60); L("sleep done"); }
	const char *names[] = {"", "1 FS statfs", "2 data-protection classes", "3 MobileKeyBag", "4 AppleKeyStore user client", "5 Keychain", "6 MobileGestalt", "7 Containers"};
	for (int s = 1; s <= 7; s++) {
		L("");
		L("######## SECTION %s ########", names[s]);
		if (s == 4) for (int i = 0; i < NCALLS; i++) run_child(self, 4, i, mobile);
		else run_child(self, s, 0, mobile);
	}
	L("==================== dptest done ====================");
	return 0;
}
