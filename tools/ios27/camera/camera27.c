/*
 * SPDX-License-Identifier: MIT-0
 *
 * The Galaxy S9's camera for iOS 27's capture stack: a library inserted into cameracaptured (the kernel's boot-arg
 * sweetinject=...;/usr/lib/cam.dylib,cameracaptured, from the root build's record: tools/ios27/fullroot/steps.d/94-camera.sh).
 * It is iOS 16.5's userland/src/inject_media.c brought to iOS 27, for the camera only; the capture device itself is the
 * same source, userland/src/camera_device.c and camera_isp.c, built into this library (tools/ios27/camera/build.sh).
 *
 * Where the camera lives on iOS 27. iOS 16.5 had the capture stack (CMCapture) in mediaserverd; iOS 27 has no
 * mediaserverd: launchd's com.apple.cameracaptured job starts /usr/libexec/cameracaptured (user mobile), which links
 * CMCapture and starts its servers (FigCaptureSessionServerStart, FigCaptureSourceServerStart, FigFlashlightServerStart,
 * the viewfinder's ...) and nothing else. The vendor of capture devices is made once, on first use, by
 * +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:] (+sharedCaptureDeviceVendor
 * calls it with none), which takes, when given none, FigCaptureGetCaptureDeviceCreateFunction()'s: that looks in IOKit
 * for Apple's camera driver (fcc_probeCameraDriverService; on an iPhone 11 AppleH10CamIn) and takes the create function
 * of the ISP's plug-in (H10ISPCaptureDeviceCreate of /System/Library/MediaCapture/H10ISP.mediacapture, which on iOS 27 is
 * in the shared cache). The S9 has no such driver, so there is no create function, no device, and no camera. (Read from
 * iOS 27.0.1's CMCapture, 17 November 2026, by disassembly: names only.)
 *
 * What this library does, only in cameracaptured and only where the kernel has made /dev/hakios-camera (boot-arg
 * cam_s9=7, drivers/camera/exynos/hakios.exynos_camera.c); anywhere else it does nothing at all:
 *   - the vendor's class method is given camera_device.c's create function (hakios_camera_device_create) when it is
 *     asked with none, in place of the ISP plug-in's that is not there. iOS 16.5's vendor took it through
 *     -initWithDeviceCreateFunction:, which iOS 27 has renamed (-initWithDefaultDeviceCreateFunction:...); the class
 *     method is the one place every caller goes through.
 *   - the capture device's derived objects: iOS 27's H10ISP device and stream classes have the shape camera_device.c
 *     builds (CMDerivedObjectCreate with FigCaptureDeviceGetClassID's class: the base class version 1; the device class
 *     version 3, its four functions ProcessingSessionCreate, RequestStreams, RelinquishStreams, PerformPearlCalibration;
 *     the stream class version 2, Start, Stop, AddAttachmentsToSampleBuffer: kH10ISPCaptureDeviceVTable and
 *     kH10ISPCaptureStreamVTable as laid out in iOS 27.0.1), and every property name camera_device.c answers is a
 *     string of iOS 27's CMCaptureCore, CMCaptureDevice or CMCapture. The formats are N104's (camera_device.c,
 *     HAKIOS_CAMERA_N104).
 *   - the same refusals as on 16.5, for the same hardware this phone does not have, each on a method whose name and
 *     arguments are as on 16.5 in iOS 27.0.1 (checked here by the number of arguments and the return type before it is
 *     replaced; a method that differs is left alone and said):
 *       +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:]  skipped (no portrait here)
 *       -[BWAudioSourceNode start:]                     a 'nope' (no capture audio) taken as started
 *       -[FigVideoCaptureConnectionConfiguration videoStabilizationMethod], -videoStabilizationType   0: none (no ISP)
 *       -[FigCaptureStillImageSettings qualityPrioritization]   at most 1 (one frame, no SIS bracket)
 *       -[BWGNRNode renderSampleBuffer:forInput:]       the picture passed on as it came (no noise reduction to do)
 *       -[BWNodeOutput emitSampleBuffer:]               a NULL buffer (the silent audio source's) not passed on
 *     and VCPHEVC.videocodec's software HEVC encoder registered (HEIC photographs), as inject_media.c explains.
 *   - What inject_media.c does with addresses of iOS 16.5's shared cache (the Metal device taken out of the process,
 *     the flashlight's own vendor) is not here: those addresses mean nothing on iOS 27, and the flashlight goes through
 *     this device's back stream anyway (camera_device.c: TorchLevel).
 * Every line it says goes to the console (the kernel's log), prefixed "hakios: camera:" (camera_device.c's) or
 * "SCIONCAM:" (this file's).
 *
 * The trace (cam_trace=1 on the boot line, or the file /private/var/hakios/camera/trace; camera_trace.c): every call the
 * device, its streams and the vendor get, said with its answer, and the program's os_log taken (camera_oslog.c), so that one
 * boot names what stops a session. Also in Camera.app when the root's record inserts this library there too
 * (94-camera.sh, CAMERA_TRACE_APP=1): there only the app's os_log is taken, nothing is replaced. Off: as before.
 *
 * NOT RUN ON A PHONE. Built for iOS 27 and loaded into nothing yet: whether cameracaptured's capture graph asks the
 * device for anything 16.5's did not, and whether Camera.app's session builds, are what the phone test is for
 * (docs/ios27/CAMERA.md).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_oslog.h"
#include "camera_trace.h"

#define PROGRAM         "cameracaptured"
#define APP             "Camera"        /* Camera.app's executable, where the record names it (the trace only) */
#define CAMERA_NODE     "/dev/hakios-camera"

typedef int (*device_create_fn)(const void *allocator, const void *options, void **out);
extern int hakios_camera_device_create(const void *allocator, const void *options, void **out);   /* camera_device.c */

static void
say(const char *format, ...)
{
	static int console = -1;
	char line[300];
	va_list ap;
	int n = snprintf(line, sizeof(line), "SCIONCAM[%d]: ", getpid());

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)n);
}

/* ---- the vendor ---------------------------------------------------------------------------------------------- */

static IMP original_vendor_with_function;

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:] */
static id
vendor_with_function(id self, SEL selector, device_create_fn function)
{
	static bool said;
	id vendor;

	if (function == NULL) {
		function = hakios_camera_device_create;
		if (!said) {
			say("the capture device vendor is made with the S9's camera device (camera_device.c) in place of the ISP "
			    "plug-in's, which this phone has none of");
		}
	}
	vendor = ((id (*)(id, SEL, device_create_fn))original_vendor_with_function)(self, selector, function);
	if (!said) {
		said = true;
		say("the capture device vendor: %s", vendor != nil ? "made" : "NONE (the capture stack refused it)");
	}
	return vendor;
}

/* ---- what the capture stack asks of hardware this phone has none of (inject_media.c's, unchanged) ------------- */

static void
prewarm_portrait_filter(id self, SEL selector, unsigned version, BOOL styles)
{
	(void)self, (void)selector, (void)version, (void)styles;
}

#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */
static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : nil;
	static bool said;

	if (started || made == nil || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = nil;
	if (!said) {
		said = true;
		say("there is no capture audio: the session's audio source gives nothing, and the camera runs without it");
	}
	return YES;
}

static int
no_stabilization(id self, SEL selector)
{
	(void)self, (void)selector;
	return 0;
}

static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket", asked);
	}
	return asked > 1 ? 1 : asked;
}

static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("a photograph goes past the noise reduction, which has nothing to do it with here");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static IMP original_emit_sample_buffer;

static void
emit_sample_buffer(id self, SEL selector, void *sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("a node's output was given no sample buffer (the silent audio source's): not passed on");
		}
		return;
	}
	scioncam_flow_emit(self, sample_buffer);
	((void (*)(id, SEL, void *))original_emit_sample_buffer)(self, selector, sample_buffer);
}

static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("no software HEVC encoder to register (%s): photographs in HEIC will fail", codec == NULL ?
		    "no VCPHEVC.videocodec" : create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without it");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	int status = register_encoder('hvc1', info, create);

	CFRelease(info);
	say("the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)", status);
}

/* ---- the methods replaced, each when its class is there ------------------------------------------------------ */

static struct replacement {
	const char *class_name, *selector;
	bool instance;
	unsigned arguments;             /* method_getNumberOfArguments(): self and _cmd included */
	char returns;                   /* the first character of its return type's encoding */
	IMP replacement, *original;
	bool done;
} replacements[] = {
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:", false, 3, '@',
	  (IMP)vendor_with_function, &original_vendor_with_function, false },
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", false, 4, 'v',
	  (IMP)prewarm_portrait_filter, NULL, false },
	{ "BWAudioSourceNode", "start:", true, 3, 'B', (IMP)audio_source_start, &original_audio_source_start, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", true, 2, 'i', (IMP)no_stabilization, NULL, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", true, 2, 'i', (IMP)no_stabilization, NULL, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", true, 2, 'i', (IMP)quality_prioritization,
	  &original_quality_prioritization, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", true, 4, 'v', (IMP)gnr_render, NULL, false },
	{ "BWNodeOutput", "emitSampleBuffer:", true, 3, 'v', (IMP)emit_sample_buffer, &original_emit_sample_buffer, false },
};
#define REPLACEMENTS (sizeof(replacements) / sizeof(replacements[0]))

static pthread_mutex_t replace_lock = PTHREAD_MUTEX_INITIALIZER;

static void
replace_methods(void)
{
	pthread_mutex_lock(&replace_lock);
	for (size_t i = 0; i < REPLACEMENTS; i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method m;
		char type[8] = "";

		if (r->done || (cls = objc_getClass(r->class_name)) == Nil) {
			continue;
		}
		r->done = true;
		m = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		if (m != NULL) {
			method_getReturnType(m, type, sizeof(type));
		}
		if (m == NULL || method_getNumberOfArguments(m) != r->arguments || type[0] != r->returns) {
			say("%c[%s %s] is not as on iOS 27.0.1 (%s): left alone", r->instance ? '-' : '+', r->class_name, r->selector,
			    m == NULL ? "not there" : "other arguments or return type");
			continue;
		}
		if (r->original != NULL) {
			*r->original = method_getImplementation(m);
		}
		method_setImplementation(m, r->replacement);
		say("%c[%s %s] replaced", r->instance ? '-' : '+', r->class_name, r->selector);
	}
	pthread_mutex_unlock(&replace_lock);
}

/* every image, those already there and each one after (CMCapture is linked by cameracaptured, so it is there) */
static int trace_level;

static void
image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header, (void)slide;
	replace_methods();
	scioncam_layout_fallback();
	scioncam_no_thumbnails();
	scioncam_colour_info();
	if (trace_level > 0) {
		scioncam_trace_vendor();
		scioncam_trace_state();
		scioncam_trace_flow();
	}
}

/* in Camera.app, with the trace: AVFoundation's cameras said (camera_apptrace.c) once AVFCapture is there */
static void
app_image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header, (void)slide;
	scioncam_trace_app();
}

/* in SpringBoard, with the trace: FrontBoardServices' layout publisher said (camera_sbtrace.c) */
static void
sb_image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header, (void)slide;
	scioncam_trace_layout_publisher();
}

/* the trace's start in this program: its os_log taken, and said */
static void
trace_start(const char *name)
{
	int hooked = scioncam_oslog_start(name, trace_level);

	say("in %s: the trace is on (cam_trace=%d): every call of the camera's device, streams and vendor said; os_log to the "
	    "console and /private/var/local/oslog/%s.log (%d of 4 entry points%s)", name, trace_level, name, hooked % 10,
	    hooked >= 10 ? ", info and debug on" : "");
}

__attribute__((constructor))
static void
scion_camera_init(void)
{
	const char *name = getprogname();

	if (name != NULL && strcmp(name, "SpringBoard") == 0) {
		/* only with the trace, where the record names it (94-camera.sh, CAMERA_TRACE_SB=1): the layout publisher traced */
		if ((trace_level = scioncam_trace_level()) > 0) {
			say("in SpringBoard: the trace is on (cam_trace=%d): FrontBoard's display layout publisher said", trace_level);
			_dyld_register_func_for_add_image(sb_image_added);
		}
		return;
	}
	if (name != NULL && strcmp(name, APP) == 0) {
		if ((trace_level = scioncam_trace_level()) > 0) {
			trace_start(name);
			_dyld_register_func_for_add_image(app_image_added);
		}
		return;
	}
	if (name == NULL || strcmp(name, PROGRAM) != 0) {
		return;
	}
	if (access(CAMERA_NODE, F_OK) != 0) {
		say("in %s: the kernel has no camera (" CAMERA_NODE "; boot-arg cam_s9=7): nothing replaced, the capture "
		    "stack is as it was", name);
		return;
	}
	say("in %s: the kernel has a camera (" CAMERA_NODE "): the capture stack gets the S9's camera device", name);
	if ((trace_level = scioncam_trace_level()) > 0) {
		hakios_camera_trace = 1;
		trace_start(name);
	}
	register_hevc_encoder();
	_dyld_register_func_for_add_image(image_added);
}
