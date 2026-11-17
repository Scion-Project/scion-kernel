/*
 * SPDX-License-Identifier: MIT-0
 *
 * The camera's trace in cameracaptured, the second part (docs/ios27/CAMERA.md, "The trace"): how the capture session
 * server decides whether a client application is in the foreground. Phone 1's boot of 17 November 21:30 had Camera.app's
 * session interrupted at once with AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableInBackground, while the app
 * was on the screen (RunningBoard: running-active-Visible). The server's FigCaptureClientApplicationStateMonitor joins
 * RunningBoard's state, the main display's layout from FrontBoard (FigCaptureDisplayLayoutMonitor; at that boot its first
 * layout "timed out ... publishing nil") and the device's lock state.
 *
 * Wrapped, each after a check of its arguments and return type (iOS 27.0.1's CMCapture, 24A446's shared cache, class-dump
 * of names and types only): the display layout monitor's updates (the layout as CMCapture describes it), the state
 * monitor's layout and lock-state callbacks and its updateApplicationState, each followed by the monitor's own
 * description. Lines: "hakios: camera: trace: state: ...", the first 80. Nothing is changed. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

#define LINES   80

static int lines;

/* an object's -debugDescription (or its description), cut at CAP */
static void
describe(id object, char *out, size_t cap)
{
	SEL debug = sel_registerName("debugDescription");
	id text = object == nil ? nil : class_respondsToSelector(object_getClass(object), debug) ?
	    ((id (*)(id, SEL))objc_msgSend)(object, debug) : (id)CFCopyDescription((CFTypeRef)object);
	CFIndex used = 0;

	out[0] = '\0';
	if (text != nil && CFGetTypeID((CFTypeRef)text) == CFStringGetTypeID()) {
		CFStringRef s = (CFStringRef)text;

		CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingUTF8, '?', false, (UInt8 *)out,
		    (CFIndex)cap - 1, &used);
		out[used] = '\0';
		for (char *c = out; *c != '\0'; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	if (object != nil && text != nil && !class_respondsToSelector(object_getClass(object), debug)) {
		CFRelease((CFTypeRef)text);
	}
	if (out[0] == '\0') {
		snprintf(out, cap, "%s", object == nil ? "nil" : "?");
	}
}

static bool
more(void)
{
	return __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < LINES;
}

static void
say_object(const char *what, id object)
{
	char text[900];

	if (more()) {
		describe(object, text, sizeof(text));
		say("trace: state: %s: %s", what, text);
	}
}

static void (*o_layout_update)(id, SEL, id);
static id (*o_parse)(id, SEL, id);
static void (*o_monitor_layout)(id, SEL, id, id);
static void (*o_monitor_lock)(id, SEL, id, bool);
static void (*o_monitor_update)(id, SEL);

static void
w_layout_update(id self, SEL sel, id layout)
{
	say_object("the display layout monitor publishes", layout);
	o_layout_update(self, sel, layout);
}

static id
w_parse(id self, SEL sel, id fbs_layout)
{
	say_object("FrontBoard's display layout arrives", fbs_layout);
	return o_parse(self, sel, fbs_layout);
}

static void
w_monitor_layout(id self, SEL sel, id monitor, id layout)
{
	o_monitor_layout(self, sel, monitor, layout);
	say_object("a client's monitor took a layout; now", self);
}

static void
w_monitor_lock(id self, SEL sel, id monitor, bool locked)
{
	o_monitor_lock(self, sel, monitor, locked);
	if (more()) {
		say("trace: state: a client's monitor took the device lock state: %s", locked ? "LOCKED" : "unlocked");
	}
	say_object("now", self);
}

static void
w_monitor_update(id self, SEL sel)
{
	o_monitor_update(self, sel);
	say_object("a client's monitor updated its application state", self);
}

static const struct wrap {
	const char *class, *selector;
	unsigned arguments;
	char returns;
	IMP wrapper, *original;
} wraps[] = {
	{ "FigCaptureDisplayLayoutMonitor", "_parseFBSDisplayLayout:", 3, '@', (IMP)w_parse, (IMP *)&o_parse },
	{ "FigCaptureDisplayLayoutMonitor", "_updateObserversWithLayout:", 3, 'v', (IMP)w_layout_update, (IMP *)&o_layout_update },
	{ "FigCaptureClientApplicationStateMonitor", "layoutMonitor:didUpdateLayout:", 4, 'v', (IMP)w_monitor_layout,
	  (IMP *)&o_monitor_layout },
	{ "FigCaptureClientApplicationStateMonitor", "deviceLockStateMonitor:didUpdateDeviceLockState:", 4, 'v',
	  (IMP)w_monitor_lock, (IMP *)&o_monitor_lock },
	{ "FigCaptureClientApplicationStateMonitor", "updateApplicationState", 2, 'v', (IMP)w_monitor_update,
	  (IMP *)&o_monitor_update },
};

void
scioncam_trace_state(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	unsigned wrapped = 0;

	pthread_mutex_lock(&once_lock);
	if (done || objc_getClass("FigCaptureClientApplicationStateMonitor") == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
		Class c = objc_getClass(wraps[i].class);
		Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(wraps[i].selector)) : NULL;
		char type[8] = "";

		if (m != NULL) {
			method_getReturnType(m, type, sizeof(type));
		}
		if (m == NULL || method_getNumberOfArguments(m) != wraps[i].arguments || type[0] != wraps[i].returns) {
			say("trace: state: -[%s %s] is not as on iOS 27.0.1 (%s): not traced", wraps[i].class, wraps[i].selector,
			    m == NULL ? "not there" : "other arguments or return type");
			continue;
		}
		*wraps[i].original = method_getImplementation(m);
		method_setImplementation(m, wraps[i].wrapper);
		wrapped++;
	}
	pthread_mutex_unlock(&once_lock);
	say("trace: state: %u of %zu methods traced", wrapped, sizeof(wraps) / sizeof(wraps[0]));
}
