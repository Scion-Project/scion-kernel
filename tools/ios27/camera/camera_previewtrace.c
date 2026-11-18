/*
 * SPDX-License-Identifier: MIT-0
 *
 * Whether the capture session's configuration has a video preview sink (cam_trace=1, cameracaptured; called with the flow
 * trace).
 *
 * Why: phone 1 with dylib -23 (18 November) had the graph running with 21 source node outputs, of which only the still
 * image output (to the still image coordinator) and the detected objects (to face detection) are connected: no node is
 * attached to the preview output, so no preview sink pipeline was built. The session server builds one for each of the
 * parsed configuration's preview sink configurations that has a video preview sink connection
 * (-[FigCaptureSessionParsedConfiguration parsedPreviewSinkConfigurations], -[FigCaptureSessionParsedPreviewSinkConfiguration
 * videoPreviewSinkConnectionConfiguration], and the camera source's own -videoPreviewSinkConnectionConfiguration; iOS 27.0.1's
 * CMCapture, 24A446: names only). These are said here: how many preview sinks the configuration has, and for each its sink
 * ID, source device type and whether it has a video preview connection. None, or none with a connection, means Camera.app's
 * session did not ask for a preview the server took; one with a connection means the graph's builder dropped it.
 * Each is said the first time and whenever it changes. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

static id
call0(id o, const char *sel)
{
	SEL s = sel_registerName(sel);

	return o == nil || !class_respondsToSelector(object_getClass(o), s) ? nil : ((id (*)(id, SEL))objc_msgSend)(o, s);
}

static void
line_of(id o, char *out, size_t cap)
{
	CFStringRef d = o == nil ? NULL : CFCopyDescription((CFTypeRef)o);

	snprintf(out, cap, "nil");
	if (d != NULL) {
		CFStringGetCString(d, out, (CFIndex)cap, kCFStringEncodingUTF8);
		CFRelease(d);
	}
	for (char *c = out; *c; c++) {
		*c = *c == '\n' || *c == '\t' ? ' ' : *c;
	}
}

static id (*o_sinks)(id, SEL), (*o_source_preview)(id, SEL);
static long last_count = -2;
static int last_source = -2;

static id
w_sinks(id self, SEL sel)
{
	id sinks = o_sinks(self, sel);
	long n = sinks != nil && CFGetTypeID((CFTypeRef)sinks) == CFArrayGetTypeID() ? (long)CFArrayGetCount((CFArrayRef)sinks) : -1;

	if (n != last_count) {
		last_count = n;
		say("trace: preview: the session's configuration has %ld preview sink(s)", n);
		for (long i = 0; i < n && i < 4; i++) {
			id p = (id)CFArrayGetValueAtIndex((CFArrayRef)sinks, i);
			char id_[64], type[48], conn[240];

			line_of(call0(p, "sinkID"), id_, sizeof(id_));
			line_of(call0(p, "sourceDeviceType"), type, sizeof(type));
			line_of(call0(p, "videoPreviewSinkConnectionConfiguration"), conn, sizeof(conn));
			say("trace: preview: sink %ld: ID %s, source device type %s, video preview connection %.200s", i, id_, type, conn);
		}
	}
	return sinks;
}

static id
w_source_preview(id self, SEL sel)
{
	id c = o_source_preview(self, sel);

	if ((c != nil) != last_source) {
		last_source = c != nil;
		say("trace: preview: the camera source's video preview sink connection: %s", c != nil ? "there" : "NONE");
	}
	return c;
}

static void
wrap(const char *class_name, const char *selector, IMP with, id (**original)(id, SEL))
{
	Class c = objc_getClass(class_name);
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(selector)) : NULL;
	char type[8] = "";

	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (m == NULL || method_getNumberOfArguments(m) != 2 || type[0] != '@') {
		say("trace: preview: -[%s %s] is not as on iOS 27.0.1: not traced", class_name, selector);
		return;
	}
	*original = (id (*)(id, SEL))method_getImplementation(m);
	method_setImplementation(m, with);
}

void
scioncam_trace_preview(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;

	pthread_mutex_lock(&once_lock);
	if (!done && objc_getClass("FigCaptureSessionParsedConfiguration") != Nil) {
		done = true;
		wrap("FigCaptureSessionParsedConfiguration", "parsedPreviewSinkConfigurations", (IMP)w_sinks, &o_sinks);
		wrap("FigCaptureSessionParsedCameraSourceConfiguration", "videoPreviewSinkConnectionConfiguration",
		    (IMP)w_source_preview, &o_source_preview);
		say("trace: preview: the session's preview sinks traced");
	}
	pthread_mutex_unlock(&once_lock);
}
