// SPDX-License-Identifier: MIT-0
// Diagnostic (docs/ios27/PACING.md): when does backboardd's CoreAnimation render thread wake and sleep? Injected by sweetinject
// into backboardd. 8 s after load: every thread's name to /dev/console once; a CFRunLoop observer added (via _CFRunLoopGet0) to the run
// loop of each thread whose name has "coreanimation" (W woke, Z about to sleep, arg = thread index); every 2 s "BD: <first abs> <us><L><arg> ..."
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
extern CFRunLoopRef _CFRunLoopGet0(pthread_t t);
#define N 65536
static struct { uint64_t at; int32_t arg; char what; } ring[N];
static volatile uint32_t wr, rd;
static int con = -1;
static void ev(char w, int32_t a) { uint32_t i = __atomic_fetch_add(&wr, 1, __ATOMIC_RELAXED); if (i - rd >= N) return; ring[i % N].at = mach_absolute_time(); ring[i % N].arg = a; ring[i % N].what = w; }
static void obs(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info) { ev(a == kCFRunLoopAfterWaiting ? 'W' : 'Z', (int32_t)(intptr_t)info); }
static void say(const char *s) { if (con >= 0) write(con, s, strlen(s)); }
static void flush(void) {
	uint32_t w = __atomic_load_n(&wr, __ATOMIC_ACQUIRE); if (w - rd > N) w = rd + N;
	if (w == rd || con < 0) return;
	char line[1024]; int n = 0; uint64_t base = ring[rd % N].at; mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	n = snprintf(line, sizeof line, "BD: %llu", base);
	for (uint32_t i = rd; i < w; i++) {
		uint64_t us = (ring[i % N].at - base) * tb.numer / tb.denom / 1000;
		n += snprintf(line + n, sizeof line - n, " %llu%c%d", us, ring[i % N].what, ring[i % N].arg);
		if (n > 900) { line[n++] = '\n'; write(con, line, n); n = snprintf(line, sizeof line, "BD: %llu", base); usleep(3000); }
	}
	line[n++] = '\n'; write(con, line, n);
	__atomic_store_n(&rd, w, __ATOMIC_RELEASE);
}
/* SpringBoard's pd.dylib (mobile: no console) writes /private/var/tmp/pd.log: its new lines are passed on to the console */
static off_t relay_at;
static void relay(void) {
	int fd = open("/private/var/tmp/pd.log", O_RDONLY); if (fd < 0) return;
	char buf[16384]; ssize_t n;
	while ((n = pread(fd, buf, sizeof buf, relay_at)) > 0) {
		ssize_t end = n; while (end > 0 && buf[end - 1] != '\n') end--;
		if (end == 0) break;
		for (ssize_t i = 0; i < end; ) { ssize_t k = i; while (k < end && buf[k] != '\n') k++; write(con, buf + i, k + 1 - i); i = k + 1; if (((i >> 10) & 3) == 0) usleep(500); }
		relay_at += end;
	}
	close(fd);
}
static void attach(void) {
	thread_act_array_t th; mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) { say("BD: task_threads failed\n"); return; }
	char line[1024]; int len = snprintf(line, sizeof line, "BD: %u threads:", n);
	for (unsigned i = 0; i < n; i++) {
		pthread_t p = pthread_from_mach_thread_np(th[i]); char name[64] = "";
		if (p) pthread_getname_np(p, name, sizeof name);
		if (len < 900) len += snprintf(line + len, sizeof line - len, " [%u %s]", i, name[0] ? name : "-");
		if (p && strstr(name, "coreanimation")) {
			CFRunLoopRef rl = _CFRunLoopGet0(p);
			CFRunLoopObserverContext ctx = { 0, (void *)(intptr_t)i, NULL, NULL, NULL };
			CFRunLoopObserverRef o = CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting, true, 0, obs, &ctx);
			CFRunLoopAddObserver(rl, o, kCFRunLoopCommonModes);
			CFArrayRef modes = CFRunLoopCopyAllModes(rl); char ms[300] = ""; int ml = 0;
			for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++) {
				CFStringRef md = CFArrayGetValueAtIndex(modes, k); char mn[96] = "";
				CFStringGetCString(md, mn, sizeof mn, kCFStringEncodingUTF8);
				if (!CFEqual(md, kCFRunLoopDefaultMode)) CFRunLoopAddObserver(rl, o, md);
				if (ml < 280) ml += snprintf(ms + ml, sizeof ms - ml, " %s", mn);
			}
			if (modes) CFRelease(modes);
			CFRunLoopWakeUp(rl);
			char s[512]; snprintf(s, sizeof s, "BD: observer on thread %u '%s' run loop %p, modes:%s\n", i, name, rl, ms); say(s);
		}
	}
	line[len++] = '\n'; line[len] = 0; say(line);
}
__attribute__((constructor)) static void bd_init(void) {
	con = open("/dev/console", O_WRONLY | O_NOCTTY);
	char s[96]; snprintf(s, sizeof s, "BD: loaded into %s pid %d\n", getprogname(), getpid()); say(s);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ attach(); });
	static dispatch_source_t t; t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(t, ^{ flush(); relay(); });
	dispatch_resume(t);
}
