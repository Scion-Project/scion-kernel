/*
 * SPDX-License-Identifier: MIT-0
 *
 * The camera's trace in Camera.app (cam_trace=N with the library in the app; docs/ios27/CAMERA.md, "The trace"): what
 * AVFoundation in the app makes of the cameras, said where the app asks a camera whether it takes a session preset.
 *
 * -[AVCaptureFigVideoDevice supportsAVCaptureSessionPreset:] (the cameras' own) is wrapped (checked first: 3 arguments, returns BOOL, as in iOS
 * 27.0.1's AVFCapture). Each answer is said, "hakios: camera: trace: app: <camera> takes <preset>: yes|NO", the first 40
 * of them; and, the first time a camera is asked, the number of its formats and each format as AVFoundation describes it
 * (its size, its frame rates; at most 30 lines a camera). No formats there means the capture stack in cameracaptured did
 * not find the stream formats N104's plist names among the formats the streams offer (camera_formats.c). Nothing is
 * changed: the answer is the original's. NOT RUN ON A PHONE.
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

#define ANSWERS         40      /* the answers said */
#define FORMATS         30      /* the formats said, a camera */
#define CAMERAS         4       /* the cameras whose formats are said */

static BOOL (*o_supports)(id, SEL, id);

/* an object's description (CFCopyDescription asks an Objective-C object its -description) */
static void
text_of(id object, char *out, size_t cap)
{
	CFStringRef d = object == nil ? NULL : CFGetTypeID((CFTypeRef)object) == CFStringGetTypeID() ?
	    CFRetain((CFStringRef)object) : CFCopyDescription((CFTypeRef)object);
	CFIndex used = 0;

	if (d != NULL) {        /* cut at CAP, not refused */
		CFStringGetBytes(d, CFRangeMake(0, CFStringGetLength(d)), kCFStringEncodingUTF8, '?', false, (UInt8 *)out,
		    (CFIndex)cap - 1, &used);
		CFRelease(d);
	}
	out[used] = '\0';
	if (used == 0) {
		snprintf(out, cap, "%s", object != nil ? "?" : "nil");
	}
}

static void
say_formats(id device, const char *camera)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static id said[CAMERAS];
	bool first = true;
	unsigned slot = CAMERAS;

	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < CAMERAS; i++) {
		if (said[i] == device) {
			first = false;
		}
		if (said[i] == nil && slot == CAMERAS) {
			slot = i;
		}
	}
	if (first && slot < CAMERAS) {
		said[slot] = device;
	}
	pthread_mutex_unlock(&lock);
	if (!first || slot == CAMERAS) {
		return;
	}
	id formats = ((id (*)(id, SEL))objc_msgSend)(device, sel_registerName("formats"));
	long n = formats != nil ? (long)CFArrayGetCount((CFArrayRef)formats) : 0;
	char text[400];

	say("trace: app: %s has %ld format(s)%s", camera, n, n == 0 ? ": NONE (cameracaptured found none of N104's stream "
	    "formats among the streams' SupportedFormatsArray)" : "");
	for (long i = 0; i < n && i < FORMATS; i++) {
		text_of((id)CFArrayGetValueAtIndex((CFArrayRef)formats, i), text, sizeof(text));
		say("trace: app: %s format %ld: %s", camera, i, text);
	}
}

static BOOL
w_supports(id self, SEL sel, id preset)
{
	static int answers;
	BOOL yes = o_supports(self, sel, preset);
	char camera[80], name[100];
	id localized = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("localizedName"));

	text_of(localized, camera, sizeof(camera));
	say_formats(self, camera);
	if (__atomic_fetch_add(&answers, 1, __ATOMIC_RELAXED) < ANSWERS) {
		text_of(preset, name, sizeof(name));
		say("trace: app: %s takes %s: %s", camera, name, yes ? "yes" : "NO");
	}
	return yes;
}

void
scioncam_trace_app(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class device;
	const char *selector = "supportsAVCaptureSessionPreset:";

	pthread_mutex_lock(&once_lock);
	if (done || (device = objc_getClass("AVCaptureFigVideoDevice")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	Method m = class_getInstanceMethod(device, sel_registerName(selector));
	char type[8] = "";

	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (m == NULL || method_getNumberOfArguments(m) != 3 || (type[0] != 'B' && type[0] != 'c')) {
		say("trace: app: -[AVCaptureFigVideoDevice %s] is not as on iOS 27.0.1 (%s): not traced", selector,
		    m == NULL ? "not there" : "other arguments or return type");
	} else {
		o_supports = (BOOL (*)(id, SEL, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)w_supports);
		say("trace: app: -[AVCaptureFigVideoDevice %s] traced", selector);
	}
	pthread_mutex_unlock(&once_lock);
}
