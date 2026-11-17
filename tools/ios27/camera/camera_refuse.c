/*
 * SPDX-License-Identifier: MIT-0
 *
 * What cameracaptured's sessions are built without on the S9 (iOS 27, with the kernel's camera): video thumbnails and the
 * Live Photo movie.
 *
 * 1. Video thumbnails.
 * Camera.app asks its session for video thumbnails (the live previews of its filters and styles). The capture session
 * server builds a FigCaptureVideoThumbnailSinkPipeline for each, fed by the preview pipeline's videoThumbnailSinkOutput.
 * With the S9's capture device the preview pipeline makes no such output, the thumbnail pipeline is built on nil, and
 * cameracaptured aborts: "NSInvalidArgumentException: Output can't be nil" (-[FigCaptureVideoThumbnailSinkPipeline
 * _buildVideoFilterThumbnailPipeline:videoSourceOutput:...], called from _captureSession_buildGraphWithConfiguration;
 * phone 1, 17 November 23:18). Every time the app retried, cameracaptured died again.
 *
 * -[FigCaptureSessionParsedConfiguration videoThumbnailSinkConfigurations] is replaced (checked first: 2 arguments,
 * returns an object): it answers an empty array, so the session is built without thumbnail pipelines. The preview, the
 * photographs and video are not affected; the filter picker shows no live thumbnails. Said once.
 *
 * 2. The Live Photo movie. With the thumbnails gone (phone 1, 23:31), cameracaptured aborted the same way ("Output can't be
 * nil") building the movie file sink's scaler (_mfsp_buildMovieFileScalerNode): Photo mode's Live Photo records a movie
 * around the shutter from the camera source pipeline's video capture output, and none was made for it. On iOS 16.5 Live
 * Photo was switched off too (rc.jobs.photo: it needs the video and the sound around the shutter). -[FigCaptureIrisSinkConfiguration
 * irisMovieCaptureEnabled] and -momentCaptureMovieRecordingEnabled answer NO (checked: 2 arguments, BOOL): the photograph
 * is taken without its movie. Said once. NOT RUN ON A PHONE.
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

static bool (*o_iris_movie)(id, SEL), (*o_moment_movie)(id, SEL);

static bool
w_no_movie(id self, SEL sel)
{
	static int said;
	bool asked = (sel == sel_registerName("irisMovieCaptureEnabled") ? o_iris_movie : o_moment_movie)(self, sel);

	if (asked && __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) == 0) {
		say("live photo: the session asked for the Live Photo movie (%s): the photograph is taken without it", sel_getName(sel));
	}
	return false;
}

static void
replace_bool(Class c, const char *selector, bool (**original)(id, SEL))
{
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(selector)) : NULL;
	char type[8] = "";

	if (m != NULL) {
		method_getReturnType(m, type, sizeof(type));
	}
	if (m == NULL || method_getNumberOfArguments(m) != 2 || (type[0] != 'B' && type[0] != 'c')) {
		say("live photo: -[FigCaptureIrisSinkConfiguration %s] is not as on iOS 27.0.1: left alone", selector);
		return;
	}
	*original = (bool (*)(id, SEL))method_getImplementation(m);
	method_setImplementation(m, (IMP)w_no_movie);
	say("live photo: -[FigCaptureIrisSinkConfiguration %s] replaced (NO)", selector);
}

static void
no_live_photo_movie(void)
{
	static bool done;
	Class c = objc_getClass("FigCaptureIrisSinkConfiguration");

	if (done || c == Nil) {
		return;
	}
	done = true;
	replace_bool(c, "irisMovieCaptureEnabled", &o_iris_movie);
	replace_bool(c, "momentCaptureMovieRecordingEnabled", &o_moment_movie);
}

void
scioncam_no_thumbnails(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c;
	const char *selector = "videoThumbnailSinkConfigurations";

	pthread_mutex_lock(&once_lock);
	no_live_photo_movie();
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
