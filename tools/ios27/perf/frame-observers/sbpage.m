// SPDX-License-Identifier: MIT-0
// Diagnostic (docs/ios27/PACING.md): when does SpringBoard move its page, and when does its main thread wake and sleep?
// Injected by sweetinject into SpringBoard. Events (mach_absolute_time) into a ring; every 2 s the new ones go to /dev/console as
// "PD: <first abs> <delta-us><letter><arg> ..." : O page scroll view offset set (arg x in points), W main run loop woke, Z about to sleep
// (W/Z always). As mobile (no /dev/console): /private/var/tmp/pd.log instead.
#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <QuartzCore/QuartzCore.h>
#include <objc/message.h>
/* frame rate requests (CAFrameRateRange) seen in SpringBoard, written as text lines; with the boot-arg pd_raise=1 a range whose
 * most is under 60 is raised to 60 (most and preferred), as the 16.5 line's sb_fps did (userland/src/inject_sb.c) */
typedef struct { float minimum, maximum, preferred; } fr_t;
static int raise60; static unsigned said;
static void tl(const char *fmt, ...);
static fr_t fr_fix(fr_t r) {
	if (raise60 && r.maximum > 0 && r.maximum < 60) { r.maximum = 60; r.preferred = 60; }
	if (raise60 && r.preferred > 0 && r.preferred < 60) r.preferred = 60;   /* pd_raise=1: no request prefers under the panel's rate */
	return r;
}
static void (*o_dl)(id, SEL, fr_t); static void h_dl(id self, SEL c, fr_t r) { if (said++ < 400) tl("RANGE link %p %.0f..%.0f pref %.0f", self, r.minimum, r.maximum, r.preferred); o_dl(self, c, fr_fix(r)); }
static void (*o_dlf)(id, SEL, long); static void h_dlf(id self, SEL c, long f) { if (said++ < 400) tl("FPS link %p %ld", self, f); o_dlf(self, c, (raise60 && f > 0 && f < 60) ? 60 : f); }
static void (*o_an)(id, SEL, fr_t); static void h_an(id self, SEL c, fr_t r) { if (said++ < 400) tl("RANGE anim %s %.0f..%.0f pref %.0f", object_getClassName(self), r.minimum, r.maximum, r.preferred); o_an(self, c, fr_fix(r)); }
static void (*o_sf)(id, SEL, fr_t, unsigned); static void h_sf(id self, SEL c, fr_t r, unsigned why) { if (said++ < 400) tl("RANGE settings %s %.0f..%.0f pref %.0f reason %u", object_getClassName(self), r.minimum, r.maximum, r.preferred, why); o_sf(self, c, fr_fix(r), why); }
static void (*o_sa)(id, SEL, fr_t, unsigned); static void h_sa(id self, SEL c, fr_t r, unsigned why) { if (said++ < 400) tl("RANGE asettings %s %.0f..%.0f pref %.0f reason %u", object_getClassName(self), r.minimum, r.maximum, r.preferred, why); o_sa(self, c, fr_fix(r), why); }
#define N 32768
static struct { uint64_t at; int32_t arg; char what; } ring[N];
static volatile uint32_t wr, rd;
static uint64_t last_o;
static uint64_t one_s;
static int con = -1;
static void ev(char w, int32_t a) { uint32_t i = __atomic_fetch_add(&wr, 1, __ATOMIC_RELAXED); if (i - rd >= N) return; ring[i % N].at = mach_absolute_time(); ring[i % N].arg = a; ring[i % N].what = w; }
static void (*orig_sco)(id, SEL, CGPoint);
static void my_sco(id self, SEL _cmd, CGPoint p) {
	CGPoint before = ((UIScrollView *)self).contentOffset;
	orig_sco(self, _cmd, p);
	if (p.x != before.x && ((UIScrollView *)self).bounds.size.width >= 300) { last_o = mach_absolute_time(); ev('O', (int32_t)p.x); }
}
static void (*orig_sb)(id, SEL, CGRect);
static void my_sb(id self, SEL _cmd, CGRect r) {
	CGRect before = ((UIView *)self).bounds;
	orig_sb(self, _cmd, r);
	if (r.origin.x != before.origin.x && r.size.width >= 300) { last_o = mach_absolute_time(); ev('B', (int32_t)r.origin.x); }
}
static void obs(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info) {
	uint64_t now = mach_absolute_time();
	(void)now;
	ev(a == kCFRunLoopAfterWaiting ? 'W' : 'Z', 0);
}
#include <stdarg.h>
/* text lines go to a ring too (no console write on the main thread); the flush timer writes them */
static char tlr[1024][200]; static volatile uint32_t tlw, tlrd;
static void tl(const char *fmt, ...) {
	uint32_t i = __atomic_fetch_add(&tlw, 1, __ATOMIC_RELAXED); if (i - tlrd >= 1024) return;
	char *b = tlr[i % 1024]; int n = snprintf(b, 200, "PDT: %llu ", mach_absolute_time());
	va_list ap; va_start(ap, fmt); n += vsnprintf(b + n, 200 - n - 2, fmt, ap); va_end(ap); if (n > 197) n = 197; b[n++] = '\n'; b[n] = 0;
}
static void tl_flush(void) {
	uint32_t w = __atomic_load_n(&tlw, __ATOMIC_ACQUIRE); if (w - tlrd > 1024) w = tlrd + 1024;
	for (uint32_t i = tlrd; i < w; i++) { if (con >= 0) write(con, tlr[i % 1024], strlen(tlr[i % 1024])); }
	__atomic_store_n(&tlrd, w, __ATOMIC_RELEASE);
}
static void hook(const char *cls, const char *sel, IMP h, IMP *o) {
	Class c = objc_getClass(cls); Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
	if (!m) { tl("no -[%s %s]", cls, sel); return; }
	*o = method_setImplementation(m, h); tl("hooked -[%s %s] %s", cls, sel, method_getTypeEncoding(m));
}
static void flush(void) {
	uint32_t w = __atomic_load_n(&wr, __ATOMIC_ACQUIRE); if (w - rd > N) w = rd + N;
	if (w == rd || con < 0) return;
	char line[1024]; int n = 0; uint64_t base = ring[rd % N].at; mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	n = snprintf(line, sizeof line, "PD: %llu", base);
	for (uint32_t i = rd; i < w; i++) {
		uint64_t us = (ring[i % N].at - base) * tb.numer / tb.denom / 1000;
		n += snprintf(line + n, sizeof line - n, " %llu%c%d", us, ring[i % N].what, ring[i % N].arg);
		if (n > 900) { line[n++] = '\n'; write(con, line, n); n = snprintf(line, sizeof line, "PD: %llu", base); usleep(2000); }
	}
	line[n++] = '\n'; write(con, line, n);
	__atomic_store_n(&rd, w, __ATOMIC_RELEASE);
}
__attribute__((constructor)) static void pd_init(void) {
	mach_timebase_info_data_t tb; mach_timebase_info(&tb); one_s = 1000000000ull * tb.denom / tb.numer;
	con = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (con < 0) con = open("/private/var/tmp/pd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);   /* SpringBoard runs as mobile: no console */
	{ char a[4096] = ""; size_t l = sizeof a - 1; if (sysctlbyname("kern.bootargs", a, &l, NULL, 0) == 0) raise60 = strstr(a, "pd_raise=1") != NULL; }
	tl("pd: raise60 %d", raise60);
	hook("CADisplayLink", "setPreferredFrameRateRange:", (IMP)h_dl, (IMP *)&o_dl);
	hook("CADisplayLink", "setPreferredFramesPerSecond:", (IMP)h_dlf, (IMP *)&o_dlf);
	hook("CAAnimation", "setPreferredFrameRateRange:", (IMP)h_an, (IMP *)&o_an);
	hook("SBFFluidBehaviorSettings", "setFrameRateRange:highFrameRateReason:", (IMP)h_sf, (IMP *)&o_sf);
	hook("SBFAnimationSettings", "setFrameRateRange:highFrameRateReason:", (IMP)h_sa, (IMP *)&o_sa);
	Class c = objc_getClass("UIScrollView");
	Method m = class_getInstanceMethod(c, @selector(setContentOffset:)); tl("UIScrollView setContentOffset: %p, own %d", m, m && class_getInstanceMethod(class_getSuperclass(c), @selector(setContentOffset:)) != m); orig_sco = (void *)method_getImplementation(m); method_setImplementation(m, (IMP)my_sco);
	(void)my_sb; (void)orig_sb;
	CFRunLoopObserverRef o = CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting, true, 2000001, obs, NULL);
	CFRunLoopAddObserver(CFRunLoopGetMain(), o, kCFRunLoopCommonModes);
	static dispatch_source_t t; t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(t, ^{ tl_flush(); flush(); });
	dispatch_resume(t);
	if (con >= 0) { char s[128]; int n = snprintf(s, sizeof s, "PD: loaded into %s pid %d\n", getprogname(), getpid()); write(con, s, n); }
}
