/*
 * SPDX-License-Identifier: MIT-0
 *
 * The preview stitcher's picture transfer, traced (cam_trace=1, with camera_flowtrace.c; docs/ios27/CAMERA.md, "The preview
 * stitcher writes nothing").
 *
 * Phone 2's boot of 22 November (PR 400's pixel trace) had the source node's pictures lit and the stitcher's output surface
 * unchanged from the first sample to the 300th: no step of the stitcher writes it. On iOS 27.2.1 (24B156) the single-camera
 * stitcher does no Metal of its own: -[BWPreviewStitcherRenderer _transferPixelBuffer:...] hands the picture to
 * -[BWPixelBufferTransferRenderer doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:
 * destinationRect:blackFillUnalignedEdges:], which for an uncompressed 420f picture calls VideoToolbox's
 * VTPixelRotationSessionRotateSubImage (rotation and scale in one). VideoToolbox builds a chain up to three times, each time
 * allowing one way less: the M2M scaler (IOSurfaceAccelerator; the S9 has none), its Metal transfer session (only when
 * VTPixelTransferSessionCanUseMetalInTheBackground: proc_can_use_foreground_hw(getpid()) answering 1, or 0 with reason 13
 * or 16), software. The stitcher's renderer ignores an error (a debug assertion only), so a failure leaves the
 * surface as it was. No texture over the camera's surface was made in cameracaptured, so the Metal chain did not run.
 * Which of the others happens, and why, is not visible from the desk.
 *
 * Here, at the transfer's 1st, 30th and 300th call (then every 3000th), and at the first call that returns each new
 * status: the status, whether there is a session, the source and destination (surface, size, format) and rectangles, the
 * destination's grid before and after (camera_pixeltrace.c), and, on the calling thread, what proc_can_use_foreground_hw
 * answers (with its reason) and VideoToolbox's own VTPixelTransferSessionCanUseMetalInTheBackground and
 * VTAvoidHardwarePixelTransfer where they are exported. Once, whether IOSurfaceAcceleratorCreate makes an accelerator.
 * Reading only: the transfer itself is the original's, called once with its own arguments. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

int proc_can_use_foreground_hw(int pid, uint32_t *reason);                                  /* libsystem_kernel, private */

typedef int (*rotate_fn)(id, SEL, void *, CVPixelBufferRef, CGRect, CVPixelBufferRef, CGRect, BOOL);
static rotate_fn o_rotate;
static _Atomic unsigned long long calls;

/* the picture's surface, size and format, as "surface 292 1504x1128 '420f'" */
static void
picture_text(CVPixelBufferRef pb, char *out, size_t cap)
{
	if (pb == NULL || CFGetTypeID(pb) != CVPixelBufferGetTypeID()) {
		snprintf(out, cap, "no picture");
		return;
	}
	IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
	uint32_t pf = CVPixelBufferGetPixelFormatType(pb);

	snprintf(out, cap, "surface %u %zux%zu '%c%c%c%c'", s != NULL ? IOSurfaceGetID(s) : 0, CVPixelBufferGetWidth(pb),
	    CVPixelBufferGetHeight(pb), pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255);
}

/* what VideoToolbox's choice of its Metal chain rests on, asked on the calling thread (the thread's voucher counts) */
static void
gate_text(char *out, size_t cap)
{
	static bool (*can_metal)(void), (*avoid_hw)(void);
	static atomic_bool looked;
	uint32_t reason = 0xffffffff;
	int r = proc_can_use_foreground_hw(getpid(), &reason);
	char m[16] = "not exported", a[16] = "not exported";

	if (!atomic_load(&looked)) {
		can_metal = (bool (*)(void))dlsym(RTLD_DEFAULT, "VTPixelTransferSessionCanUseMetalInTheBackground");
		avoid_hw = (bool (*)(void))dlsym(RTLD_DEFAULT, "VTAvoidHardwarePixelTransfer");
		atomic_store(&looked, true);
	}
	if (can_metal != NULL) {
		snprintf(m, sizeof(m), "%s", can_metal() ? "yes" : "no");
	}
	if (avoid_hw != NULL) {
		snprintf(a, sizeof(a), "%s", avoid_hw() ? "yes" : "no");
	}
	snprintf(out, cap, "proc_can_use_foreground_hw %d reason %u; VideoToolbox: Metal in the background %s, avoid hardware %s",
	    r, reason, m, a);
}

/* once: whether this process can make an IOSurfaceAccelerator (the M2M scaler VideoToolbox's hardware chain needs) */
static void
accelerator_ask(void)
{
	void *lib = dlopen("/System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator", RTLD_LAZY);
	int (*create)(CFAllocatorRef, CFDictionaryRef, CFTypeRef *) =
	    lib != NULL ? (int (*)(CFAllocatorRef, CFDictionaryRef, CFTypeRef *))dlsym(lib, "IOSurfaceAcceleratorCreate") : NULL;
	CFTypeRef acc = NULL;
	int r;

	if (create == NULL) {
		say("trace: stitch: IOSurfaceAcceleratorCreate is not there");
		return;
	}
	r = create(kCFAllocatorDefault, NULL, &acc);
	say("trace: stitch: IOSurfaceAcceleratorCreate: %d (%#x), accelerator %s", r, (unsigned)r, acc != NULL ? "made" : "none");
	if (acc != NULL) {
		CFRelease(acc);
	}
}

/* a status not said before (8 at most): the first call that returns it is said in full */
static bool
fresh_status(int status)
{
	static int seen[8];
	static unsigned n;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	bool fresh = true;

	pthread_mutex_lock(&lock);
	for (unsigned k = 0; k < n; k++) {
		fresh = fresh && seen[k] != status;
	}
	fresh = fresh && n < 8;
	if (fresh) {
		seen[n++] = status;
	}
	pthread_mutex_unlock(&lock);
	return fresh;
}

static int
w_rotate(id self, SEL _cmd, void *session, CVPixelBufferRef src, CGRect src_rect, CVPixelBufferRef dst, CGRect dst_rect,
    BOOL black_fill)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	unsigned long long n = atomic_fetch_add(&calls, 1) + 1;
	bool due = scioncam_pixels_due(n);
	char gate[200] = "", where[80], s[96], d[96];
	int status;

	if (due) {
		gate_text(gate, sizeof(gate));
		snprintf(where, sizeof(where), "the stitcher's transfer %llu, destination before", n);
		scioncam_pixels_of(where, dst);
	}
	status = o_rotate(self, _cmd, session, src, src_rect, dst, dst_rect, black_fill);
	if (!fresh_status(status) && !due) {
		return status;
	}
	if (!due) {
		gate_text(gate, sizeof(gate));          /* after the call: the same thread, so the same answer */
	}
	pthread_once(&once, accelerator_ask);
	picture_text(src, s, sizeof(s));
	picture_text(dst, d, sizeof(d));
	say("trace: stitch: transfer %llu: status %d, session %s; from %s rect %g,%g %gx%g to %s rect %g,%g %gx%g, "
	    "black fill %s", n, status, session != NULL ? "there" : "NONE", s, src_rect.origin.x, src_rect.origin.y,
	    src_rect.size.width, src_rect.size.height, d, dst_rect.origin.x, dst_rect.origin.y, dst_rect.size.width,
	    dst_rect.size.height, black_fill ? "yes" : "no");
	say("trace: stitch: transfer %llu: %s", n, gate);
	if (due) {
		snprintf(where, sizeof(where), "the stitcher's transfer %llu, destination after", n);
		scioncam_pixels_of(where, dst);
	}
	return status;
}

/* -[BWPreviewStitcherRenderer renderStitchedPixelBuffer:fromFramePairInfo:alignmentCoordinator:transitionCoordinator:
   renderParameters:renderInfoOut:] (24B156: the node ignores what it returns; the parameters go by reference): its 1st,
   30th and 300th call said with how many transfers had been made by then, so a stitcher that renders without transferring
   shows as such */
typedef long (*render_fn)(id, SEL, CVPixelBufferRef, id, id, id, void *, void *);
static render_fn o_render;
static _Atomic unsigned long long renders;

static long
w_render(id self, SEL _cmd, CVPixelBufferRef dst, id pair, id alignment, id transition, void *parameters, void *info)
{
	unsigned long long n = atomic_fetch_add(&renders, 1) + 1;
	long r = o_render(self, _cmd, dst, pair, alignment, transition, parameters, info);

	if (scioncam_pixels_due(n)) {
		char d[96];

		picture_text(dst, d, sizeof(d));
		say("trace: stitch: render %llu into %s: %llu transfers so far", n, d, atomic_load(&calls));
	}
	return r;
}

/* with the flow trace, once CMCapture is there (idempotent) */
void
scioncam_trace_stitch(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c = objc_getClass("BWPixelBufferTransferRenderer");
	SEL sel = sel_registerName("doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:"
	    "blackFillUnalignedEdges:");

	pthread_mutex_lock(&lock);
	if (!done && c != Nil) {
		Method m = class_getInstanceMethod(c, sel);
		char t[8] = "";

		done = true;
		if (m != NULL) {
			method_getReturnType(m, t, sizeof(t));
		}
		if (m == NULL || method_getNumberOfArguments(m) != 8 || (t[0] != 'i' && t[0] != 'I')) {
			say("trace: stitch: -[BWPixelBufferTransferRenderer doPixelRotationTransferWithSession:...] is not as on "
			    "24B156 (%s, return '%s'): not traced", m == NULL ? "not there" : "other arguments or return type", t);
		} else {
			o_rotate = (rotate_fn)method_getImplementation(m);
			method_setImplementation(m, (IMP)w_rotate);
			say("trace: stitch: the preview stitcher's transfer (VTPixelRotationSession) traced");
		}
		Class rc = objc_getClass("BWPreviewStitcherRenderer");
		Method r = rc != Nil ? class_getInstanceMethod(rc, sel_registerName("renderStitchedPixelBuffer:fromFramePairInfo:"
		    "alignmentCoordinator:transitionCoordinator:renderParameters:renderInfoOut:")) : NULL;

		t[0] = 0;
		if (r != NULL) {
			method_getReturnType(r, t, sizeof(t));
		}
		if (r == NULL || method_getNumberOfArguments(r) != 8 || strchr("{(fd", t[0]) != NULL) {
			say("trace: stitch: -[BWPreviewStitcherRenderer renderStitchedPixelBuffer:...] is not as on 24B156: not traced");
		} else {
			o_render = (render_fn)method_getImplementation(r);
			method_setImplementation(r, (IMP)w_render);
		}
	}
	pthread_mutex_unlock(&lock);
}
