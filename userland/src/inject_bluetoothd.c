/*
 * libhakios.bluetoothd.dylib: bluetoothd on the Galaxy S9 (build.sh BT=1, which also puts it into bluetoothd through
 * etc/inject.conf).
 *
 * When an accessory offers iAP over Bluetooth (the Sony WH-1000XM3 does, on an RFCOMM channel), bluetoothd 16.5 opens
 * the "Wireless IAP" profile as soon as the device is paired: it makes a PTY (openpty: "Tried to open PTY with central
 * = 17, peripheral = 18"), hands the peripheral end to the accessory server (com.apple.accessories.transport-server,
 * accessoryd: not on this system, "Could not establish connection to IAP") and from then on writes what the
 * accessory sends over iAP into the central end -- from its stack thread, holding the stack's mutex. Nobody reads the
 * other end, so after a few hundred bytes the PTY is full and that write() sleeps in the kernel ("ptcout") for good;
 * 20 s later bluetoothd's own watchdog finds the mutex still taken ("Stack Mutex try lock failed and watchdog has
 * struck twice -- abort"), bluetoothd exits and the headphones are dropped (phone 1, 3 November, 15:26).
 *
 * bluetoothd itself makes the central end non-blocking in one of its configurations (after openpty: fcntl(F_GETFL),
 * "Flags are 0x%08x", fcntl(F_SETFL, | O_NONBLOCK)), only not in the one this phone gets. So here, in bluetoothd and
 * nowhere else, openpty's central end is always made non-blocking: a write the PTY has no room for fails (EAGAIN) and
 * is dropped instead of stopping the stack. iAP itself goes nowhere (there is no accessory server); A2DP, AVRCP and
 * HFP do not use the PTY.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <crt_externs.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <stdarg.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

/*
 * bluetoothd died of SIGSEGV while its stack stopped for a power-off from Settings ("Terminating stack...", 15 November), and this
 * phone has no crash reporter: the daemon says where it was (as inject_media.c's does for mediaserverd), the fault's address, the
 * frames from pc, lr and the frame pointers' chain, each as image, symbol, offset in the image, and the cache address. Then the
 * default action is put back and the fault happens again.
 */
extern const void *_dyld_get_shared_cache_range(size_t *length);
static uintptr_t slide;

static void
crash_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uintptr_t pc = 0, lr = 0, fp = 0, lo, hi;
	pthread_t self = pthread_self();
	char thread[64] = "?";

	pthread_getname_np(self, thread, sizeof(thread));
	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	}
	hi = (uintptr_t)pthread_get_stackaddr_np(self);
	lo = hi - pthread_get_stacksize_np(self);
	say("hakios: bluetoothd: CRASH, signal %d at address %p on thread '%s'; frames follow\n", sig, info != NULL ? info->si_addr : NULL,
	    thread);
	for (int i = 0; i < 40; i++) {
		uintptr_t addr = i == 0 ? pc : i == 1 ? lr : 0;
		Dl_info d;

		if (i >= 2) {
			if (fp < lo || fp + 16 > hi || (fp & 15) != 0) {
				break;
			}
			addr = ((uintptr_t *)fp)[1];
			fp = ((uintptr_t *)fp)[0];
		}
		if (addr == 0) {
			continue;
		}
		if (dladdr((void *)addr, &d) && d.dli_fname != NULL) {
			const char *base = strrchr(d.dli_fname, '/');

			say("hakios: bluetoothd:   #%d %s %s+%lu (image offset %#lx, cache address %#lx)\n", i, base ? base + 1 : d.dli_fname,
			    d.dli_sname ? d.dli_sname : "?", d.dli_sname ? (unsigned long)(addr - (uintptr_t)d.dli_saddr) : 0UL,
			    (unsigned long)(addr - (uintptr_t)d.dli_fbase), (unsigned long)(addr - slide));
		} else {
			say("hakios: bluetoothd:   #%d %#lx\n", i, (unsigned long)addr);
		}
	}
	signal(sig, SIG_DFL);
}

/*
 * The power-off from Settings: bluetoothd's stack shutdown (0x1000f3970 of 20F66's bluetoothd) calls the HCI transport's
 * entry at +96 (0x1000e656c: "if the transport's first word is not 0, branch to its +96") without looking whether it is
 * there -- and the transport this phone gets, "H4" (a UART: BT_HCI_TRANSPORT unset, "Searching for transport: \"H4\""), has
 * first word 1 and NULL at +96 (the iPhones use PCIE, whose table has it). bluetoothd jumped to 0 at every power-off
 * (SIGSEGV at address 0, step 87, 16 November), was started again by launchd, and the switch in Settings took that long.
 * The entry is given a function that does nothing and answers 0, in the H4 table only, and only if it is what was read here:
 * the table at 0x100844218 named "H4", first word 1, +96 NULL.
 */
static int
h4_nothing(void)
{
	return 0;
}

static void
h4_fill(void)
{
	/* bluetoothd's own slide: its header (the main executable's), not image 0's -- step 88 read a wrong address with that */
	intptr_t slide = (intptr_t)_NSGetMachExecuteHeader() - (intptr_t)0x100000000UL;
	uint8_t *table = (uint8_t *)(0x100844218UL + (uintptr_t)slide);
	const char *name = *(const char **)(table + 8);
	void **entry = (void **)(table + 96);
	uintptr_t lo = 0x100000000UL + (uintptr_t)slide, hi = lo + 0x900000;

	if ((uintptr_t)name < lo || (uintptr_t)name >= hi) {
		say("libhakios.bluetoothd: the H4 transport's name pointer %p is not in bluetoothd: its +96 left alone\n", name);
		return;
	}
	if (strcmp(name, "H4") != 0 || *(uint32_t *)table != 1 || *entry != NULL) {
		say("libhakios.bluetoothd: the H4 transport's table is not as read (%s, %u, %p): its +96 left alone\n",
		    name != NULL ? name : "?", *(uint32_t *)table, *entry);
		return;
	}
	*entry = (void *)h4_nothing;
	say("libhakios.bluetoothd: the H4 transport's +96 (called at power-off, NULL in 20F66) given a function that does nothing\n");
}

__attribute__((constructor)) static void
hello(void)
{
	struct sigaction action;
	static const int signals[] = { SIGTRAP, SIGSEGV, SIGBUS, SIGILL, SIGABRT };
	size_t length = 0;

	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = crash_handler;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	say("libhakios.bluetoothd: in bluetoothd (pid %d): openpty's central end non-blocking; the crash reporter of ours armed\n", getpid());
	/* two seconds on (no power-off comes that soon): the table is read once bluetoothd is running, not from inside dyld */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
		h4_fill();
	});
}

static int
my_openpty(int *central, int *peripheral, char *name, struct termios *termp, struct winsize *winp)
{
	int r = openpty(central, peripheral, name, termp, winp);
	int saved = errno;

	if (r == 0 && central != NULL) {
		int flags = fcntl(*central, F_GETFL);

		if (flags >= 0 && fcntl(*central, F_SETFL, flags | O_NONBLOCK) == 0) {
			say("libhakios.bluetoothd: PTY %s: central end %d non-blocking (nothing reads the other end here)\n",
			    name != NULL ? name : "?", *central);
		}
	}
	errno = saved;
	return r;
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_openpty, (const void *)openpty },
};
