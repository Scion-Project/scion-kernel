/*
 * touchspy: an IOHIDEventSystem monitor client on the phone's iOS root. Logs
 * every digitizer event (and each child contact) as the HID event system in
 * backboardd sees it, after its filters, plus a line per button event so the
 * log shows the client works. Output: /private/var/tmp/touchspy.log.
 * Built by tools/touchspy/build-touchspy.sh; run as launchd job
 * com.starbite.touchspy (tools/touchspy/install-touchspy.py).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef uint32_t IOHIDEventField;
typedef void (*IOHIDEventSystemClientEventCallback)(void *target, void *refcon,
    IOHIDServiceClientRef sender, IOHIDEventRef event);

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef, int type, CFDictionaryRef);
extern void IOHIDEventSystemClientScheduleWithRunLoop(IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern void IOHIDEventSystemClientRegisterEventCallback(IOHIDEventSystemClientRef,
    IOHIDEventSystemClientEventCallback, void *, void *);
extern void IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef, CFDictionaryRef);
extern uint32_t IOHIDEventGetType(IOHIDEventRef);
extern CFIndex IOHIDEventGetIntegerValue(IOHIDEventRef, IOHIDEventField);
extern double IOHIDEventGetFloatValue(IOHIDEventRef, IOHIDEventField);
extern uint64_t IOHIDEventGetSenderID(IOHIDEventRef);
extern uint64_t IOHIDEventGetTimeStamp(IOHIDEventRef);
extern uint32_t IOHIDEventGetEventFlags(IOHIDEventRef);
extern CFArrayRef IOHIDEventGetChildren(IOHIDEventRef);

#define TYPE_KEYBOARD   3
#define TYPE_DIGITIZER  11
#define F(n)            ((TYPE_DIGITIZER << 16) | (n))
#define KB(n)           ((TYPE_KEYBOARD << 16) | (n))

static FILE *out;
static unsigned long nevents, ndigi;

static void
contact(const char *tag, IOHIDEventRef e)
{
	fprintf(out, "  %s x=%.4f y=%.4f z=%.2f btn=%ld type=%ld idx=%ld id=%ld mask=0x%lx range=%ld touch=%ld "
	    "press=%.2f qual=%.2f dens=%.2f maj=%.2f min=%.2f coll=%ld cmask=0x%lx integ=%ld gen=%ld "
	    "will=0x%lx did=0x%lx flags=0x%x\n", tag,
	    IOHIDEventGetFloatValue(e, F(0)), IOHIDEventGetFloatValue(e, F(1)), IOHIDEventGetFloatValue(e, F(2)),
	    (long)IOHIDEventGetIntegerValue(e, F(3)), (long)IOHIDEventGetIntegerValue(e, F(4)),
	    (long)IOHIDEventGetIntegerValue(e, F(5)), (long)IOHIDEventGetIntegerValue(e, F(6)),
	    (long)IOHIDEventGetIntegerValue(e, F(7)), (long)IOHIDEventGetIntegerValue(e, F(8)),
	    (long)IOHIDEventGetIntegerValue(e, F(9)), IOHIDEventGetFloatValue(e, F(10)),
	    IOHIDEventGetFloatValue(e, F(17)), IOHIDEventGetFloatValue(e, F(18)),
	    IOHIDEventGetFloatValue(e, F(20)), IOHIDEventGetFloatValue(e, F(21)),
	    (long)IOHIDEventGetIntegerValue(e, F(22)), (long)IOHIDEventGetIntegerValue(e, F(24)),
	    (long)IOHIDEventGetIntegerValue(e, F(25)), (long)IOHIDEventGetIntegerValue(e, F(27)),
	    (long)IOHIDEventGetIntegerValue(e, F(28)), (long)IOHIDEventGetIntegerValue(e, F(29)),
	    IOHIDEventGetEventFlags(e));
}

static void
callback(void *target, void *refcon, IOHIDServiceClientRef sender, IOHIDEventRef e)
{
	(void)target; (void)refcon; (void)sender;
	uint32_t type = IOHIDEventGetType(e);
	nevents++;
	if (type == TYPE_KEYBOARD) {
		fprintf(out, "key t=%llu sender=0x%llx page=0x%lx usage=0x%lx down=%ld\n",
		    (unsigned long long)IOHIDEventGetTimeStamp(e), (unsigned long long)IOHIDEventGetSenderID(e),
		    (long)IOHIDEventGetIntegerValue(e, KB(0)), (long)IOHIDEventGetIntegerValue(e, KB(1)),
		    (long)IOHIDEventGetIntegerValue(e, KB(2)));
	} else if (type == TYPE_DIGITIZER) {
		if (++ndigi > 4000) {
			return;
		}
		CFArrayRef kids = IOHIDEventGetChildren(e);
		CFIndex n = kids ? CFArrayGetCount(kids) : 0;
		fprintf(out, "digitizer #%lu t=%llu sender=0x%llx children=%ld\n", ndigi,
		    (unsigned long long)IOHIDEventGetTimeStamp(e), (unsigned long long)IOHIDEventGetSenderID(e), (long)n);
		contact("parent", e);
		for (CFIndex i = 0; i < n; i++) {
			IOHIDEventRef c = (IOHIDEventRef)CFArrayGetValueAtIndex(kids, i);
			char tag[32];
			snprintf(tag, sizeof(tag), "child%ld(type %u)", (long)i, IOHIDEventGetType(c));
			if (IOHIDEventGetType(c) == TYPE_DIGITIZER) {
				contact(tag, c);
			} else {
				fprintf(out, "  %s\n", tag);
			}
		}
	} else if (nevents < 200) {
		fprintf(out, "event type %u sender=0x%llx\n", type, (unsigned long long)IOHIDEventGetSenderID(e));
	}
	fflush(out);
}

int
main(void)
{
	out = fopen("/private/var/tmp/touchspy.log", "a");
	if (!out) {
		return 1;
	}
	setvbuf(out, NULL, _IOLBF, 0);
	fprintf(out, "=== touchspy start pid %d\n", getpid());
	IOHIDEventSystemClientRef client = NULL;
	/* The event system lives in backboardd; wait for it. Type 1 = monitor. */
	for (int i = 0; i < 120 && !client; i++) {
		client = IOHIDEventSystemClientCreateWithType(kCFAllocatorDefault, 1, NULL);
		if (!client) {
			sleep(2);
		}
	}
	if (!client) {
		fprintf(out, "no event system client\n");
		return 1;
	}
	fprintf(out, "client %p\n", (void *)client);
	IOHIDEventSystemClientSetMatching(client, NULL);
	IOHIDEventSystemClientRegisterEventCallback(client, callback, NULL, NULL);
	IOHIDEventSystemClientScheduleWithRunLoop(client, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	fprintf(out, "scheduled\n");
	CFRunLoopRun();
	return 0;
}
