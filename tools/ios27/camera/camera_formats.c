/*
 * SPDX-License-Identifier: MIT-0
 *
 * The formats the S9's streams offer on iOS 27 (cam.dylib, HAKIOS_CAMERA_N104), in place of camera_device.c's two.
 *
 * Why: on iOS 27 the capture stack makes the cameras' AVCaptureDevice formats, and from them the session presets, out of
 * CMCapture's N104/AVCaptureSession.plist (the iPhone 11's). Each AVCaptureDeviceFormat there names a stream format of
 * the plist's FigCaptureStreamFormats ("12MP" names "Photo", "1080p30" names "Video-FullSensor-FES-30", ...), and the
 * named format is looked for in the stream's SupportedFormatsArray. An AVCaptureDeviceFormat whose stream format is not
 * found is dropped; a preset whose format is dropped is not supported. iOS 27.0.1's CMCapture (24A446's shared cache,
 * csu_resolveFormatIndexForNamedStreamFormat, read 17 November 2026: its logic only) takes the first entry of
 * SupportedFormatsArray, Experimental ones left out, for which all of these hold:
 *   SensorWidth and SensorHeight are the plist's;
 *   VideoIsBinned is the plist's Binned; SensorHDRMode > 0 is its HDR;
 *   FrontEndScalerOutputWidth and ...Height are both > 0 exactly when the plist says FES, and then are its FESWidth and
 *   FESHeight;
 *   SIFREnabled is the plist's (and, both on, SIFRBinningFactor the same, 0 counting as 1);
 *   VideoMaxFrameRate is the plist's MaxFrameRate where it names one;
 *   QuadraBinningFactor, OptimizedForMidFrameSynchronization and SensorSigningEnabled as the plist's (none of them here).
 * The companion formats a named format lists (NonSIFRCompanionFormat, FESCompanionFormats, DepthCompanionFormat) are
 * looked for the same way; one not found is only left out.
 *
 * camera_device.c's formats had no SIFREnabled, while N104's "Photo", "Video-60" (back) and "Video-30" (front) say
 * SIFREnabled: so none of them was found, the cameras had no formats, and Camera.app said "unsupported session preset
 * AVCaptureSessionPresetPhoto" and could not add the camera to its session (phone 1, 17 November 2026, front and back).
 *
 * The entries below are N104's stream formats of the app's Photo mode and of 4K video, front and back. The formats behind
 * a front-end scaler (FES: "Video-FullSensor-FES-30", which 1080p30, 720p30 and 1920 x 1440 name) are not offered: with
 * one, Camera.app's Photo mode took 1920 x 1440 and cameracaptured threw an exception sizing an output buffer
 * (-[BWMultiStreamCameraSourceNode _bytesPerOutputBufferForOutputConfiguration:outputID:], FigCapturePixelFormat-
 * BytesPerPixel: a pixel format it does not know; phone 1, 17 November 22:49). So 1080p video has no format yet. The sizes and rates are the plist's: the S9's sensors give 1632 x 1224 (front) and
 * 2016 x 1512 (rear) at 30 frames a second, and every picture is scaled from that to what an output asks for, as before
 * (camera_device.c, camera_isp.c). A rate above 30 is only what a format may be asked for: the frames still come at 30.
 * SIFR ("secondary image frame readout") is the iPhone's sensor interleaving a second, shorter exposure; the S9 has none
 * and no such frames are made. The front camera also offers "Photo"'s NonSIFRCompanionFormat (the same size without
 * SIFR), which the stack takes when it does not want SIFR. Formats at 120 and 240 frames a second, binned ones and the
 * back camera's non-SIFR companion (4224 x 3176 at 120) are not offered. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "camera_formats.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
extern int hakios_camera_trace;                                                         /* camera_say.c */

static const struct n104_format {
	uint32_t        sensor;                 /* the kernel's: 1 the back camera, 2 the front one */
	const char      *name;                  /* N104's FigCaptureStreamFormats name, for the trace */
	int             width, height, fps;     /* SensorWidth, SensorHeight, VideoMaxFrameRate */
	bool            sifr;                   /* SIFREnabled */
	int             fes_width, fes_height;  /* FrontEndScalerOutputWidth, ...Height (0: no front-end scaler) */
} n104[] = {
	{ 1, "Photo", 4224, 3168, 60, true, 0, 0 },                             /* 12MP, 8MP, 5MP: AVCaptureSessionPresetPhoto */
	{ 1, "Video-60", 4224, 2384, 60, true, 0, 0 },                          /* UHD30, UHD60 */
	{ 2, "Photo", 4032, 3024, 30, true, 0, 0 },                             /* 12MP: AVCaptureSessionPresetPhoto */
	{ 2, "Video-30", 4032, 2272, 30, true, 0, 0 },                          /* UHD30 */
	{ 2, "Photo's NonSIFRCompanionFormat", 4032, 3024, 30, false, 0, 0 },
};

static void
put_number(CFMutableDictionaryRef d, const char *key, int value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);

	CFDictionarySetValue(d, k, n);
	CFRelease(n);
	CFRelease(k);
}

CFArrayRef
scioncam_n104_formats(uint32_t sensor, const char *camera, double fov, scioncam_format_fn make)
{
	CFMutableArrayRef formats = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	for (size_t i = 0; i < sizeof(n104) / sizeof(n104[0]); i++) {
		const struct n104_format *e = &n104[i];

		if (e->sensor != sensor) {
			continue;
		}
		/*
		 * Two entries of one camera may be the same (the back camera's 1080p30 and 1920x1440 stream formats): the first
		 * is found for both. It is offered once.
		 */
		bool same = false;

		for (size_t j = 0; j < i; j++) {
			same |= n104[j].sensor == sensor && n104[j].width == e->width && n104[j].height == e->height &&
			    n104[j].fps == e->fps && n104[j].sifr == e->sifr && n104[j].fes_width == e->fes_width &&
			    n104[j].fes_height == e->fes_height;
		}
		if (same) {
			continue;
		}
		CFDictionaryRef base = make(e->width, e->height, e->fps, false, fov);
		CFMutableDictionaryRef f = CFDictionaryCreateMutableCopy(NULL, 0, base);

		CFRelease(base);
		CFDictionarySetValue(f, CFSTR("SIFREnabled"), e->sifr ? kCFBooleanTrue : kCFBooleanFalse);
		if (e->fes_width > 0) {
			put_number(f, "FrontEndScalerOutputWidth", e->fes_width);
			put_number(f, "FrontEndScalerOutputHeight", e->fes_height);
		}
		if (hakios_camera_trace) {
			hakios_camera_say("trace: stream (%s): format %ld is N104's \"%s\": %d x %d, at most %d a second%s%s", camera,
			    (long)CFArrayGetCount(formats), e->name, e->width, e->height, e->fps, e->sifr ? ", SIFR" : "",
			    e->fes_width > 0 ? ", front-end scaler" : "");
		}
		CFArrayAppendValue(formats, f);
		CFRelease(f);
	}
	if (CFArrayGetCount(formats) == 0) {
		CFRelease(formats);
		return NULL;
	}
	return formats;
}
