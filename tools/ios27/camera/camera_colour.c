/*
 * SPDX-License-Identifier: MIT-0
 *
 * The colour of the stream's outputs, as iOS 27's capture graph checks it before it starts the stream (cameracaptured,
 * with the kernel's camera; docs/ios27/CAMERA.md, "The fourth cause").
 *
 * Why: the graph's source node (BWMultiStreamCameraSourceNode), bringing the stream up to date for a session
 * (-allocateOrReuseBufferPoolsFromSourceNode: -> _bringStreamUpToDate -> _verifyColorSpacePropertiesForAllOutputs), reads
 * the stream's VideoOutputsColorInfo once and then, for every output whose colour space properties are not 0 (the node's
 * _colorSpaceProperties for the scalers; the still image output's own, which may be another), wants that output's entry
 * to be equal (isEqualToDictionary:) to what +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] gives for
 * those properties, asked right before the comparison; anything else is error -12780 (iOS 27.0.1's CMCapture, 24A446's
 * shared cache, read 18 November 2026: its logic only; the per-output methods are direct methods, out of reach of the
 * Objective-C runtime). Apple's ISP driver answers the property with the colour attachments of what it makes for each
 * output. The S9's stream answered an empty dictionary (the graph never sets the property), so on phone 1 every session of
 * Camera.app failed at once with -12780 (the app's "session runtime error" -11800), and the app retried about thirty times
 * a second with a black viewfinder (17 November 23:49). The node's own _colorSpaceProperties was 0 when read (18 November
 * 00:15), and the sessions still failed: the output that fails is one whose properties are not the node's.
 *
 * What is done (always, in cameracaptured with the kernel's camera):
 *   - +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] is wrapped to remember, per thread, what it gave last
 *     (it is not changed);
 *   - camera_device.c asks hakios_camera_colour_info for its answer when the graph has set none: every output's entry is one
 *     object, a dictionary whose count, objectForKey: and keyEnumerator are those of what that wrapper gave last on the
 *     asking thread, so the check's comparison, made right after it asked for the attachments, finds them equal for any
 *     output and any properties;
 *   - -[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:] is wrapped (checked: 3 arguments, an int
 *     returned) to say the update's result when it fails, with the properties asked for and how many comparisons there were.
 * The pictures' own colour attachments are camera_device.c's as before (BT.601 matrix, BT.709 primaries and transfer).
 * NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

extern CFDictionaryRef (*hakios_camera_colour_info)(void);                               /* camera_device.c */

static __thread id current_node;        /* the source node whose stream is being brought up to date on this thread */
static ptrdiff_t properties_offset = -1;
static IMP o_allocate, o_attachments;

/* what +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] gave last on this thread, and what it was asked */
static __thread CFTypeRef last_attachments;
static __thread int asked, asked_props[4], compared;

static id
w_attachments(id self, SEL sel, int properties)
{
	id r = ((id (*)(id, SEL, int))o_attachments)(self, sel, properties);

	if (r != nil) {
		CFRetain((CFTypeRef)r);
	}
	if (last_attachments != NULL) {
		CFRelease(last_attachments);
	}
	last_attachments = (CFTypeRef)r;
	if (current_node != nil && asked < 4) {
		asked_props[asked] = properties;
	}
	asked += current_node != nil;
	return r;
}

/* ---- the entry every output gets: equal to the attachments just asked for ----------------------------------------- */

static CFDictionaryRef
none(void)
{
	static CFDictionaryRef empty;

	if (empty == NULL) {
		empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return empty;
}

static CFDictionaryRef
now(void)
{
	return last_attachments != NULL && CFGetTypeID(last_attachments) == CFDictionaryGetTypeID() ? last_attachments : none();
}

static unsigned long
e_count(id self, SEL sel)
{
	compared += current_node != nil;
	return (unsigned long)CFDictionaryGetCount(now());
}

static id
e_object(id self, SEL sel, id key)
{
	return key != nil ? (id)CFDictionaryGetValue(now(), (CFTypeRef)key) : nil;
}

static id
e_keys(id self, SEL sel)
{
	return ((id (*)(id, SEL))objc_msgSend)((id)now(), sel_registerName("keyEnumerator"));
}

static id
entry(void)
{
	static id one;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&lock);
	if (one == nil) {
		Class dictionary = objc_getClass("NSDictionary"), c = objc_allocateClassPair(dictionary, "SCIONCamColourEntry", 0);

		if (c != Nil) {
			class_addMethod(c, sel_registerName("count"), (IMP)e_count, "Q@:");
			class_addMethod(c, sel_registerName("objectForKey:"), (IMP)e_object, "@@:@");
			class_addMethod(c, sel_registerName("keyEnumerator"), (IMP)e_keys, "@@:");
			objc_registerClassPair(c);
			one = class_createInstance(c, 0);       /* (not -init: NSDictionary's would want a concrete initializer) */
		}
	}
	pthread_mutex_unlock(&lock);
	return one;
}

static int
w_allocate(id self, SEL sel, id source)
{
	static int said;
	id before = current_node;
	int r;

	current_node = self;
	asked = compared = 0;
	r = ((int (*)(id, SEL, id))o_allocate)(self, sel, source);
	if (r != 0 && __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 5) {
		say("colour: the source node's update FAILED (%d): node colour space properties %d; attachments asked %d time(s) (%d %d "
		    "%d %d), the colour info's entries compared %d time(s)", r,
		    properties_offset >= 0 ? *(int *)((uint8_t *)(void *)self + properties_offset) : -1, asked, asked_props[0],
		    asked_props[1], asked_props[2], asked_props[3], compared);
	}
	current_node = before;
	return r;
}

/* the outputs a stream of the S9 has, and those the graph names on iOS 27 (an entry for an output that is not checked
   is not looked at) */
static const char *const outputs[] = { "PrimaryScaler", "SecondaryScaler", "StillImage", "IntermediateTap", "Depth" };

static CFDictionaryRef
colour_info(void)
{
	static int said;
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	id e = entry();

	for (size_t i = 0; e != nil && i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		CFStringRef name = CFStringCreateWithCString(NULL, outputs[i], kCFStringEncodingUTF8);

		CFDictionarySetValue(d, name, (CFTypeRef)e);
		CFRelease(name);
	}
	if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0) {
		say("colour: the stream's colour info asked for%s: each output's entry follows the attachments the check asks for",
		    current_node != nil ? " in the source node's update" : " (not in the source node's update)");
	}
	return d;
}

void
scioncam_colour_info(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c;
	Method m;
	Ivar v;
	char type[8] = "";
	const char *selector = "allocateOrReuseBufferPoolsFromSourceNode:", *ivar = "_colorSpaceProperties";

	pthread_mutex_lock(&once_lock);
	if (done || (c = objc_getClass("BWMultiStreamCameraSourceNode")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	v = class_getInstanceVariable(c, ivar);
	m = class_getInstanceMethod(c, sel_registerName(selector));
	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (v == NULL || ivar_getTypeEncoding(v) == NULL || strchr("iI", ivar_getTypeEncoding(v)[0]) == NULL) {
		say("colour: -[BWMultiStreamCameraSourceNode %s] is not an int as on iOS 27.0.1: the stream's colour info stays empty", ivar);
	} else if (m == NULL || method_getNumberOfArguments(m) != 3 || type[0] == '\0' || type[0] != 'i') {
		say("colour: -[BWMultiStreamCameraSourceNode %s] is not as on iOS 27.0.1 (%s, %u arguments, returns '%s'): the stream's "
		    "colour info stays empty", selector, m == NULL ? "not there" : "there", m == NULL ? 0 : method_getNumberOfArguments(m), type);
	} else {
		Class format = objc_getClass("BWVideoFormat");
		Method a = format != Nil ? class_getClassMethod(format, sel_registerName("pixelBufferAttachmentsForColorSpaceProperties:")) : NULL;
		char atype[8] = "";

		if (a != NULL) {
			method_getReturnType(a, atype, sizeof(atype));
		}
		if (a == NULL || method_getNumberOfArguments(a) != 3 || atype[0] != '@' || entry() == nil) {
			say("colour: +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] is not as on iOS 27.0.1 (%s), or no "
			    "entry class: the stream's colour info stays empty", a == NULL ? "not there" : "other arguments or return type");
		} else {
			properties_offset = ivar_getOffset(v);
			o_attachments = method_getImplementation(a);
			method_setImplementation(a, (IMP)w_attachments);
			o_allocate = method_getImplementation(m);
			method_setImplementation(m, (IMP)w_allocate);
			hakios_camera_colour_info = colour_info;
			say("colour: -[BWMultiStreamCameraSourceNode %s] and +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] "
			    "wrapped: the stream's colour info follows what the graph's check asks for", selector);
		}
	}
	pthread_mutex_unlock(&once_lock);
}
