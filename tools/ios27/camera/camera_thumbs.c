/*
 * SPDX-License-Identifier: MIT-0
 *
 * No video thumbnails in cameracaptured's sessions (iOS 27, with the kernel's camera).
 *
 * Camera.app asks its session for video thumbnails (the live previews of its filters and styles). The capture session
 * server builds a FigCaptureVideoThumbnailSinkPipeline for each, fed by the preview pipeline's videoThumbnailSinkOutput.
 * With the S9's capture device the preview pipeline makes no such output, the thumbnail pipeline is built on nil, and
 * cameracaptured aborts: "NSInvalidArgumentException: Output can't be nil" (-[FigCaptureVideoThumbnailSinkPipeline
 * _buildVideoFilterThumbnailPipeline:videoSourceOutput:...], called from _captureSession_buildGraphWithConfiguration;
 * phone 1, 17 November 23:18). Every time the app retried, cameracaptured died again.
 *
 * -[FigCaptureSessionParsedConfiguration videoThumbnailSinkConfigurations] is replaced (checked first: 2 arguments,
 * returns an object): it answers an empty array, so the session is built without thumbnail pipelines. The preview, the
 * photographs and video are not affected; the filter picker shows no live thumbnails. Said once. NOT RUN ON A PHONE.
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

static id (*o_thumbnails)(id, SEL);

static id
w_thumbnails(id self, SEL sel)
{
	static int said;
	static CFArrayRef none;
	id asked = o_thumbnails(self, sel);

	if (asked == nil || CFArrayGetCount((CFArrayRef)asked) == 0) {
		return asked;
	}
	if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0) {
		say("thumbnails: the session asked for %ld video thumbnail(s): built without them (the S9's preview has no "
		    "thumbnail output)", (long)CFArrayGetCount((CFArrayRef)asked));
	}
	if (none == NULL) {
		none = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);    /* kept: autoreleased-like, never freed */
	}
	return (id)none;
}

void
scioncam_no_thumbnails(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c;
	const char *selector = "videoThumbnailSinkConfigurations";

	pthread_mutex_lock(&once_lock);
	if (done || (c = objc_getClass("FigCaptureSessionParsedConfiguration")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	Method m = class_getInstanceMethod(c, sel_registerName(selector));
	char type[8] = "";

	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (m == NULL || method_getNumberOfArguments(m) != 2 || type[0] != '@') {
		say("thumbnails: -[FigCaptureSessionParsedConfiguration %s] is not as on iOS 27.0.1: left alone", selector);
	} else {
		o_thumbnails = (id (*)(id, SEL))method_getImplementation(m);
		method_setImplementation(m, (IMP)w_thumbnails);
		say("thumbnails: -[FigCaptureSessionParsedConfiguration %s] replaced (sessions without video thumbnails)",
		    selector);
	}
	pthread_mutex_unlock(&once_lock);
}
