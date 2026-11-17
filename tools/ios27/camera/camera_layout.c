/*
 * SPDX-License-Identifier: MIT-0
 *
 * cameracaptured's view of which application is in the foreground, when FrontBoard publishes no display layout.
 *
 * The capture session server lets a client application use the camera only while it is in the foreground. Its
 * FigCaptureClientApplicationStateMonitor (iOS 27.0.1's CMCapture) joins two sources: RunningBoard's state of the
 * application (running-active-Visible, or not) and the main display's layout from FrontBoard
 * (FigCaptureDisplayLayoutMonitor, over FBSDisplayLayoutMonitor): the client must be among the layout's foreground
 * applications. On phone 1 (17 November 2026, 21:30) FrontBoard's layout service published nothing to cameracaptured
 * ("FBSDisplayLayoutService: timed out waiting for initial layout update from com.apple.frontboard.main-display;
 * publishing nil", and SpringBoard's own layout monitors saw "(null)"), so the monitor's layout stayed nil, the app was
 * never in it, and Camera.app's session was interrupted at once with VideoDeviceNotAvailableInBackground while the app
 * was on the screen.
 *
 * What this does: -[FigCaptureClientApplicationStateMonitor layoutMonitor:didUpdateLayout:] is replaced (checked first:
 * 4 arguments, returns void). When the monitor is the main display's (+[FigCaptureDisplayLayoutMonitor
 * sharedDisplayLayoutMonitor]) and the layout it gives is nil, or has no foreground application while FrontBoard's own
 * layout under it (its FBSDisplayLayoutMonitor's) is missing or names no application at all, the client is given in its
 * place a layout of that display
 * with the client's own application as its one foreground application (a FigCaptureDisplayLayout made with its own
 * -_init, -setDisplayType: and -setForegroundApps:). Every layout FrontBoard does publish goes through unchanged, and
 * RunningBoard's state still decides: an application that leaves the screen is still seen as in the background. With no
 * layout at all, two applications on the screen at once (split view, picture in picture) cannot be told apart from one;
 * an iPhone 11's system has neither split view nor another camera client on the screen in this port. Said once a
 * client ("hakios: camera: layout: ..."). Only in cameracaptured, only with the kernel's camera (cam_s9=7).
 * NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

#define SAID    8       /* the clients said */

static void (*o_did_update)(id, SEL, id, id);

static id
send(id object, const char *selector)
{
	return ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
}

/* the client's application identifier (the monitor's _client, -applicationID), or nil */
static id
client_application(id monitor)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(monitor), "_client");
	id client = ivar != NULL ? object_getIvar(monitor, ivar) : nil;

	return client != nil && class_respondsToSelector(object_getClass(client), sel_registerName("applicationID")) ?
	       send(client, "applicationID") : nil;
}

/* a layout of the display of MONITOR whose one foreground application is APP (retained), or nil */
static id
layout_with(id monitor, id app)
{
	Class layout = objc_getClass("FigCaptureDisplayLayout");
	SEL init = sel_registerName("_init"), type = sel_registerName("setDisplayType:"), apps = sel_registerName("setForegroundApps:");

	if (layout == Nil || !class_respondsToSelector(layout, init) || !class_respondsToSelector(layout, type) ||
	    !class_respondsToSelector(layout, apps)) {
		return nil;
	}
	id made = ((id (*)(id, SEL))objc_msgSend)(send((id)layout, "alloc"), init);
	long long display = ((long long (*)(id, SEL))objc_msgSend)(monitor, sel_registerName("displayType"));
	const void *one[1] = { app };
	CFArrayRef foreground = CFArrayCreate(NULL, one, 1, &kCFTypeArrayCallBacks);

	((void (*)(id, SEL, long long))objc_msgSend)(made, type, display);
	((void (*)(id, SEL, id))objc_msgSend)(made, apps, (id)foreground);
	CFRelease(foreground);
	return made;
}

/* FrontBoard's own layout under MONITOR (its FBSDisplayLayoutMonitor's -currentLayout), or nil */
static id
frontboard_layout(id monitor)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(monitor), "_layoutMonitor");
	id fbs = ivar != NULL ? object_getIvar(monitor, ivar) : nil;

	return fbs != nil && class_respondsToSelector(object_getClass(fbs), sel_registerName("currentLayout")) ?
	       send(fbs, "currentLayout") : nil;
}

/*
 * FrontBoard gave MONITOR (a FigCaptureDisplayLayoutMonitor) no application: no layout at all, or a layout none of whose
 * elements is an application's (-isUIApplicationElement). On phone 1 (17 November, 22:25) FrontBoard's monitor had a
 * layout and CMCapture found no foreground application in it while the Camera app was on the screen; CMCapture then hands
 * its clients an empty FigCaptureDisplayLayout, not nil.
 */
static bool
frontboard_gave_nothing(id monitor)
{
	id fbs = frontboard_layout(monitor), elements, all;
	SEL is_app = sel_registerName("isUIApplicationElement");

	if (fbs == nil) {
		return true;
	}
	if (!class_respondsToSelector(object_getClass(fbs), sel_registerName("elements")) ||
	    (elements = send(fbs, "elements")) == nil) {
		return true;
	}
	all = class_respondsToSelector(object_getClass(elements), sel_registerName("allObjects")) ? send(elements, "allObjects") :
	    elements;
	if (all == nil || !class_respondsToSelector(object_getClass(all), sel_registerName("objectAtIndex:"))) {
		return false;   /* not a collection this knows: as FrontBoard says */
	}
	for (CFIndex i = 0, n = CFArrayGetCount((CFArrayRef)all); i < n; i++) {
		id e = (id)CFArrayGetValueAtIndex((CFArrayRef)all, i);

		if (class_respondsToSelector(object_getClass(e), is_app) && ((bool (*)(id, SEL))objc_msgSend)(e, is_app)) {
			return false;
		}
	}
	return true;
}

static long
foreground_count(id layout)
{
	id apps = layout != nil && class_respondsToSelector(object_getClass(layout), sel_registerName("foregroundApps")) ?
	    send(layout, "foregroundApps") : nil;

	return apps != nil ? (long)CFArrayGetCount((CFArrayRef)apps) : 0;
}

static void
w_did_update(id self, SEL sel, id monitor, id layout)
{
	static id said[SAID];
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static int diagnosed;
	Class monitors = objc_getClass("FigCaptureDisplayLayoutMonitor");
	id main_display = monitors != Nil ? send((id)monitors, "sharedDisplayLayoutMonitor") : nil;
	bool is_main = monitor != nil && monitor == main_display;
	bool nothing = is_main && (layout == nil || (foreground_count(layout) == 0 && frontboard_gave_nothing(monitor)));
	id app, made;

	if (is_main && __atomic_fetch_add(&diagnosed, 1, __ATOMIC_RELAXED) < 3) {
		id fbs = frontboard_layout(monitor);
		char text[900] = "nil";
		CFStringRef d = fbs != nil ? CFCopyDescription((CFTypeRef)fbs) : NULL;
		CFIndex used = 0;

		if (d != NULL) {
			CFStringGetBytes(d, CFRangeMake(0, CFStringGetLength(d)), kCFStringEncodingUTF8, '?', false, (UInt8 *)text,
			    sizeof(text) - 1, &used);
			text[used] = '\0';
			CFRelease(d);
			for (char *c = text; *c != '\0'; c++) {
				*c = *c == '\n' ? ' ' : *c;
			}
		}
		say("layout: the main display's layout for a client: %s, %ld foreground app(s); FrontBoard's %s an application: %s",
		    layout == nil ? "nil" : "a layout", foreground_count(layout), frontboard_gave_nothing(monitor) ? "names NO" :
		    "names", text);
	}
	if (!nothing || (app = client_application(self)) == nil || (made = layout_with(monitor, app)) == nil) {
		o_did_update(self, sel, monitor, layout);
		return;
	}
	bool first = true;

	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < SAID && first; i++) {
		if (said[i] == self) {
			first = false;
		} else if (said[i] == nil) {
			said[i] = self;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	if (first) {
		char name[128] = "?";

		if (CFGetTypeID((CFTypeRef)app) == CFStringGetTypeID()) {
			CFStringGetCString((CFStringRef)app, name, sizeof(name), kCFStringEncodingUTF8);
		}
		say("layout: FrontBoard gave the main display no layout: %s is taken as its foreground application "
		    "(RunningBoard's state still decides; docs/ios27/CAMERA.md)", name);
	}
	o_did_update(self, sel, monitor, made);
	CFRelease((CFTypeRef)made);
}

void
scioncam_layout_fallback(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c;
	const char *selector = "layoutMonitor:didUpdateLayout:";

	pthread_mutex_lock(&once_lock);
	if (done || (c = objc_getClass("FigCaptureClientApplicationStateMonitor")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	Method m = class_getInstanceMethod(c, sel_registerName(selector));
	char type[8] = "";

	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (m == NULL || method_getNumberOfArguments(m) != 4 || type[0] != 'v' ||
	    objc_getClass("FigCaptureDisplayLayout") == Nil || objc_getClass("FigCaptureDisplayLayoutMonitor") == Nil ||
	    class_getClassMethod(objc_getClass("FigCaptureDisplayLayoutMonitor"), sel_registerName("sharedDisplayLayoutMonitor")) == NULL) {
		say("layout: -[FigCaptureClientApplicationStateMonitor %s] is not as on iOS 27.0.1: left alone", selector);
	} else {
		o_did_update = (void (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)w_did_update);
		say("layout: -[FigCaptureClientApplicationStateMonitor %s] replaced (a nil layout of the main display: the "
		    "client in the foreground)", selector);
	}
	pthread_mutex_unlock(&once_lock);
}
