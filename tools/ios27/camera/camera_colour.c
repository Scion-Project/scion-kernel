/*
 * SPDX-License-Identifier: MIT-0
 *
 * The colour of the stream's outputs, as iOS 27's capture graph checks it before it starts the stream (cameracaptured,
 * with the kernel's camera; docs/ios27/CAMERA.md, "The fourth cause").
 *
 * Why: the graph's source node (BWMultiStreamCameraSourceNode), bringing the stream up to date for a session
 * (-allocateOrReuseBufferPoolsFromSourceNode: -> _bringStreamUpToDate -> _verifyColorSpacePropertiesForAllOutputs), reads
 * the stream's VideoOutputsColorInfo once and then, for every output whose colour space properties are not 0, wants that
 * output's entry to be equal (isEqualToDictionary:) to +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] of
 * those properties; anything else is error -12780. iOS 27.0.1's CMCapture (24A446's shared cache, read 18 November 2026:
 * its logic only): the scalers' properties are the node's _colorSpaceProperties; any other output's (the still image
 * output's) are, when the still image output is the firmware's and wants a 10-bit pixel format, 1 ("sRGB/601") or 3
 * ("P3/601", for a wide colour space), else the node's again. Apple's ISP driver answers the property with the colour
 * attachments of what it makes for each output. The S9's stream answered an empty dictionary (the graph never sets the
 * property), so every session of Camera.app on phone 1 failed at once with -12780 (the app's "session runtime error" -11800)
 * and was retried about thirty times a second with a black viewfinder (17 November 23:49). With the node's own properties as
 * the answer the sessions still failed (18 November 00:15): they were 0 ("N/A", the scalers are not checked) and the
 * attachments of 0 are nil, so the output that fails is the still image output, at 1, sRGB/601.
 *
 * What the S9's pictures are (camera_isp.c): sRGB's curve, BT.601's matrix (420f full range, 420v video range), the
 * sensor's colours white-balanced with no colour matrix unless a tuning gives one, taken as sRGB's primaries (BT.709's):
 * sRGB/601, properties 1, which is also the only colour space the streams offer (SupportedColorSpaces: sRGB).
 *
 * What is done (always, in cameracaptured with the kernel's camera):
 *   - the stream's VideoOutputsColorInfo, when the graph has set none, is +[BWVideoFormat
 *     pixelBufferAttachmentsForColorSpaceProperties:1] for every output, and every picture camera_device.c makes carries
 *     those same attachments (hakios_camera_frame_colour): the stream says what it makes, and makes what it says;
 *   - pixelBufferAttachmentsForColorSpaceProperties: is wrapped to remember what the check asked for (it is not changed), and
 *     -[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:] (checked: 3 arguments, an int returned) to
 *     say an update that still fails, with the properties the check asked for: one boot shows both sides. A check that asks
 *     for 3 (P3/601) would mean the session wants wide colour, which the S9's pictures are not: the conversion is a known gap.
 * With cam_colour_any=1 on the boot line (a diagnostic, off by default), every output's entry is instead a dictionary that
 * reads as whatever attachments the check asked for last on that thread: a session that then runs shows that the colour
 * check was its only stop. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "scion_bootline.h"
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

extern CFDictionaryRef (*hakios_camera_colour_info)(void);                               /* camera_device.c */
extern CFDictionaryRef hakios_camera_frame_colour;                                      /* camera_device.c */

#define SRGB_601        1       /* BWColorSpaceProperties: "sRGB/601" (24A446's BWColorSpacePropertiesToString) */

static bool any_colour;         /* cam_colour_any=1: the diagnostic entries (below) */

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
		say("colour: the source node's update FAILED (%d): node colour space properties %d; attachments asked %d time(s) "
		    "in the update, for properties %d %d %d %d (1 sRGB/601, 3 P3/601; the stream answered %s); entries compared %d time(s)",
		    r, properties_offset >= 0 ? *(int *)((uint8_t *)(void *)self + properties_offset) : -1, asked, asked_props[0],
		    asked_props[1], asked_props[2], asked_props[3], any_colour ? "the diagnostic's" : "sRGB/601", compared);
	}
	current_node = before;
	return r;
}

/* the outputs a stream of the S9 has, and those the graph names on iOS 27 (an entry for an output that is not checked
   is not looked at) */
static const char *const outputs[] = { "PrimaryScaler", "SecondaryScaler", "StillImage", "IntermediateTap", "Depth" };

/* the attachments of sRGB/601: what every output of the S9's stream makes (kept: computed once) */
static CFDictionaryRef
srgb_601(void)
{
	static CFDictionaryRef kept;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	Class format = objc_getClass("BWVideoFormat");

	pthread_mutex_lock(&lock);
	if (kept == NULL && format != Nil && o_attachments != NULL) {
		id d = ((id (*)(id, SEL, int))o_attachments)((id)format, sel_registerName("pixelBufferAttachmentsForColorSpaceProperties:"),
		    SRGB_601);

		if (d != nil && CFGetTypeID((CFTypeRef)d) == CFDictionaryGetTypeID()) {
			kept = CFDictionaryCreateCopy(NULL, (CFDictionaryRef)d);
		}
	}
	pthread_mutex_unlock(&lock);
	return kept;
}

static CFDictionaryRef
colour_info(void)
{
	static int said;
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFTypeRef e = any_colour && entry() != nil ? (CFTypeRef)entry() : (CFTypeRef)srgb_601();

	if (hakios_camera_frame_colour == NULL) {
		hakios_camera_frame_colour = srgb_601();        /* (from now on every picture carries them) */
	}

	for (size_t i = 0; e != NULL && i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		CFStringRef name = CFStringCreateWithCString(NULL, outputs[i], kCFStringEncodingUTF8);

		CFDictionarySetValue(d, name, e);
		CFRelease(name);
	}
	if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0) {
		char line[400] = "nil";
		CFStringRef text = srgb_601() != NULL ? CFCopyDescription(srgb_601()) : NULL;

		if (text != NULL) {
			CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		for (char *c = line; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
		say("colour: the stream's colour info asked for%s: %s", current_node != nil ? " in the source node's update" :
		    " (not in the source node's update)", any_colour ? "cam_colour_any=1: each output's entry reads as the attachments "
		    "the check asks for (a diagnostic)" : "every output sRGB/601, as its pictures are");
		if (!any_colour || hakios_camera_trace) {
			say("colour: sRGB/601's attachments: %s", line);
		}
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
		if (a == NULL || method_getNumberOfArguments(a) != 3 || atype[0] != '@') {
			say("colour: +[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:] is not as on iOS 27.0.1 (%s): the "
			    "stream's colour info stays empty", a == NULL ? "not there" : "other arguments or return type");
		} else {
			properties_offset = ivar_getOffset(v);
			o_attachments = method_getImplementation(a);
			method_setImplementation(a, (IMP)w_attachments);
			o_allocate = method_getImplementation(m);
			method_setImplementation(m, (IMP)w_allocate);
			any_colour = scion_boot_arg_number("cam_colour_any", 0) > 0;
			hakios_camera_colour_info = colour_info;
			say("colour: the stream's colour info will be %s, and the pictures carry sRGB/601's attachments from the first "
			    "session on", any_colour ? "the diagnostic's (cam_colour_any=1)" : "sRGB/601 for every output");
		}
	}
	pthread_mutex_unlock(&once_lock);
}
