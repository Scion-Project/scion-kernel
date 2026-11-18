/*
 * SPDX-License-Identifier: MIT-0
 *
 * The colour of the stream's outputs, as iOS 27's capture graph checks it before it starts the stream (cameracaptured,
 * with the kernel's camera; docs/ios27/CAMERA.md, "The fourth cause").
 *
 * Why: the graph's source node (BWMultiStreamCameraSourceNode), bringing the stream up to date for a session
 * (-allocateOrReuseBufferPoolsFromSourceNode: -> _bringStreamUpToDate -> _verifyColorSpacePropertiesForAllOutputs), reads
 * the stream's VideoOutputsColorInfo and, for every output whose colour space properties are not 0, wants that output's
 * entry to be the dictionary +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] gives for the node's colour
 * space properties (its ivar _colorSpaceProperties); anything else is error -12780 (iOS 27.0.1's CMCapture, 24A446's
 * shared cache, read 18 November 2026: its logic only). Apple's ISP driver answers that property with the colour
 * attachments of what it makes for each output. The S9's stream answered an empty dictionary (the graph never sets the
 * property), so on phone 1 (17 November 23:49) every session of Camera.app failed at once with -12780, the app's
 * "session runtime error" -11800, and the app retried about thirty times a second with a black viewfinder.
 *
 * What is done: -[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:] is wrapped (checked: 3
 * arguments, an int returned (an OSStatus: _bringStreamUpToDate's), or an object, BOOL or nothing); while it runs, the node is
 * this thread's. camera_device.c asks
 * hakios_camera_colour_info for its answer when the graph has set none: for each of the stream's outputs, the node's
 * own pixelBufferAttachmentsForColorSpaceProperties: dictionary, which is what the check compares with. The pictures'
 * own colour attachments are camera_device.c's as before (BT.601 matrix, BT.709 primaries and transfer). NOT RUN ON A PHONE.
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
static IMP o_allocate;

static void *
w_allocate(id self, SEL sel, id source)
{
	id before = current_node;
	void *r;

	current_node = self;
	r = ((void *(*)(id, SEL, id))o_allocate)(self, sel, source);
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
	Class format = objc_getClass("BWVideoFormat");
	SEL make = sel_registerName("pixelBufferAttachmentsForColorSpaceProperties:");
	int properties;
	id attachments;

	if (current_node == nil || properties_offset < 0 || format == Nil || !class_respondsToSelector(object_getClass((id)format), make)) {
		if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0) {
			say("colour: the stream's colour info asked for outside the source node's update%s: answered empty",
			    format == Nil ? " (no BWVideoFormat)" : "");
		}
		return d;
	}
	properties = *(int *)((uint8_t *)(void *)current_node + properties_offset);
	attachments = ((id (*)(id, SEL, int))objc_msgSend)((id)format, make, properties);
	for (size_t i = 0; attachments != nil && i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		CFStringRef name = CFStringCreateWithCString(NULL, outputs[i], kCFStringEncodingUTF8);

		CFDictionarySetValue(d, name, (CFTypeRef)attachments);
		CFRelease(name);
	}
	if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0 || hakios_camera_trace) {
		CFStringRef text = attachments != nil ? CFCopyDescription((CFTypeRef)attachments) : NULL;
		char line[400] = "nil";

		if (text != NULL) {
			CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		for (char *c = line; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
		say("colour: the stream's colour info for the source node's check: colour space properties %d, each output %s",
		    properties, line);
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
	} else if (m == NULL || method_getNumberOfArguments(m) != 3 || type[0] == '\0' || strchr("iv@Bc", type[0]) == NULL) {
		say("colour: -[BWMultiStreamCameraSourceNode %s] is not as on iOS 27.0.1 (%s, %u arguments, returns '%s'): the stream's "
		    "colour info stays empty", selector, m == NULL ? "not there" : "there", m == NULL ? 0 : method_getNumberOfArguments(m), type);
	} else {
		properties_offset = ivar_getOffset(v);
		o_allocate = method_getImplementation(m);
		method_setImplementation(m, (IMP)w_allocate);
		hakios_camera_colour_info = colour_info;
		say("colour: -[BWMultiStreamCameraSourceNode %s] wrapped: the stream answers its colour info with the node's own", selector);
	}
	pthread_mutex_unlock(&once_lock);
}
