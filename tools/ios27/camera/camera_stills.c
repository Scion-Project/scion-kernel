/*
 * SPDX-License-Identifier: MIT-0
 *
 * The photograph's processing on the S9 (cam.dylib in cameracaptured, iOS 27): the photo formats' fused and deferred kinds
 * answered NO, so that a photograph takes the plain path.
 *
 * Why: phone 1 with dylib -20 (18 November) gave the still image coordinator a 4032 x 3024 picture at the shutter, and the
 * photograph still failed 10 ms after it was asked for: AVFoundationErrorDomain -11800, underlying -16808, and nothing was
 * saved. In iOS 27.0.1's CMCapture (24A446's shared cache, its logic only) -16808 is returned only by the processing-state
 * setup of BWPhotonicEngineNode and BWUBNode (the fusion of several frames: Deep Fusion, Smart HDR, deferred processing on
 * the Neural Engine), which the S9 cannot run. N104's 12MP formats say HDRSupported, DeepFusionSupported,
 * DeferredPhotoProcessingSupported and DigitalFlashSupported. The FigCaptureSourceVideoFormat methods below answer NO
 * (each checked: 2 arguments, BOOL), so the session's photographs are single frames, as iOS 16.5's were on the S9. Said
 * once each when the plist's answer was YES. cam_fusion=1 on the boot line keeps the plist's answers. The app shows no HDR
 * or night mode switch while this holds. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

static const char *const selectors[] = {
	"isHDRSupported", "isDeepFusionSupported", "isDeferredPhotoProcessingSupported", "isDigitalFlashSupported",
	"isGNRHDRSupported", "isSmartStyleRenderingSupported", "isSemanticStyleRenderingSupported",
};
#define N (sizeof(selectors) / sizeof(selectors[0]))
static bool (*originals[N])(id, SEL);
static int said[N];

static bool
w_no(id self, SEL sel)
{
	for (size_t i = 0; i < N; i++) {
		if (sel == sel_registerName(selectors[i])) {
			if (originals[i](self, sel) && __atomic_fetch_add(&said[i], 1, __ATOMIC_RELAXED) == 0) {
				say("stills: a format says %s: answered NO (a single-frame photograph)", selectors[i]);
			}
			return false;
		}
	}
	return false;
}

static bool
fusion_kept(void)
{
	char args[1024] = "", *w, *last = NULL;
	size_t n = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0) {
		return false;
	}
	args[n < sizeof(args) ? n : sizeof(args) - 1] = '\0';
	for (w = strtok_r(args, " ", &last); w != NULL; w = strtok_r(NULL, " ", &last)) {
		if (strncmp(w, "cam_fusion=", 11) == 0 && atoi(w + 11) > 0) {
			return true;
		}
	}
	return false;
}

void
scioncam_single_frame_stills(void)
{
	static bool done;
	Class c = objc_getClass("FigCaptureSourceVideoFormat");
	unsigned replaced = 0;

	if (done || c == Nil) {
		return;
	}
	done = true;
	if (fusion_kept()) {
		say("stills: cam_fusion=1: the formats' fused photographs left as the plist says");
		return;
	}
	for (size_t i = 0; i < N; i++) {
		Method m = class_getInstanceMethod(c, sel_registerName(selectors[i]));
		char type[8] = "";

		if (m != NULL) {
			method_getReturnType(m, type, sizeof(type));
		}
		if (m == NULL || method_getNumberOfArguments(m) != 2 || (type[0] != 'B' && type[0] != 'c')) {
			say("stills: -[FigCaptureSourceVideoFormat %s] is not as on iOS 27.0.1: left alone", selectors[i]);
			continue;
		}
		originals[i] = (bool (*)(id, SEL))method_getImplementation(m);
		method_setImplementation(m, (IMP)w_no);
		replaced++;
	}
	say("stills: %u of %zu fused-photograph methods of the formats answer NO", replaced, N);
}
