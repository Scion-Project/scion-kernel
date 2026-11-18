/*
 * SPDX-License-Identifier: MIT-0
 *
 * The photograph in a 10-bit buffer (cam.dylib, iOS 27): what camera_device.c draws when the graph's StillImage pool is
 * 'xf20' (kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: two planes, Y and interleaved CbCr, each sample 16 bits
 * little-endian with its 10 bits at the top).
 *
 * Why: N104's AVCaptureSession.plist says UsesPacked10BitFirmwareStillImageOutputPixelFormat for its photo formats ("12MP",
 * "7MP", 1080p, 720p, 1920 x 1440: front and back). iOS 27.0.1's CMCapture (24A446's shared cache, its logic only) then sets
 * the source node's stillImageOutputRequires10BitPixelFormat, and -[BWMultiStreamCameraSourceNode
 * _supportedPixelFormatsForOutputID:] keeps, for the still image output, only the formats of the stream's
 * SupportedPixelFormats that are full range, 10-bit (exactly when that flag is set), not 4:2:2 and not raw. The S9's streams
 * offered 420f, 420v and BGRA only: with the flag, none was left for the still image output.
 *
 * camera_refuse.c answers NO to -[FigCaptureSourceVideoFormat usesPacked10BitFirmwareStillImageOutputPixelFormat], so the
 * still image output takes 420f as every other output does. camera_formats.c also offers 'xf20' on the StillImage output,
 * which a session that still wants 10 bits then finds (with cam_still10=1 the plist's answer is kept, to try this path).
 * The picture is made as before, in 8 bits (camera_isp.c), into a scratch 420f buffer of the same size, and widened here:
 * luma v -> v * 1023 / 255 (the two top bits repeated at the bottom), chroma c -> c * 4 (128 stays the middle, 512).
 * NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */

static CVPixelBufferRef scratch;        /* the last size asked for; the still image output is drawn from one thread */

/* an 8-bit full-range buffer of pb's size to draw into, or NULL (said) */
CVPixelBufferRef
scioncam_tenbit_scratch(CVPixelBufferRef pb)
{
	size_t width = CVPixelBufferGetWidth(pb), height = CVPixelBufferGetHeight(pb);

	if (scratch != NULL && (CVPixelBufferGetWidth(scratch) != width || CVPixelBufferGetHeight(scratch) != height)) {
		CVPixelBufferRelease(scratch);
		scratch = NULL;
	}
	if (scratch == NULL) {
		CFDictionaryRef none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		const void *k[] = { kCVPixelBufferIOSurfacePropertiesKey }, *v[] = { none };
		CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		CVReturn r = CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &scratch);

		CFRelease(attrs);
		CFRelease(none);
		if (r != kCVReturnSuccess) {
			hakios_camera_say("StillImage: no %zux%zu 420f scratch buffer for the 10-bit picture (%d): no picture", width,
			    height, r);
			scratch = NULL;
			return NULL;
		}
		hakios_camera_say("StillImage: the graph's pool is 10-bit (xf20): drawn in 8 bits at %zux%zu and widened", width,
		    height);
	}
	return scratch;
}

static void
widen_plane(const uint8_t *src, size_t src_stride, uint8_t *dst, size_t dst_stride, size_t samples, size_t rows, bool luma)
{
	for (size_t r = 0; r < rows; r++) {
		const uint8_t *s = src + r * src_stride;
		uint16_t *d = (uint16_t *)(void *)(dst + r * dst_stride);

		for (size_t i = 0; i < samples; i++) {
			d[i] = luma ? (uint16_t)((s[i] << 8 | s[i]) & 0xffc0) : (uint16_t)(s[i] << 8);
		}
	}
}

/* from (420f, locked) into pb (xf20, locked), of the same size */
void
scioncam_tenbit_widen(CVPixelBufferRef from, CVPixelBufferRef pb)
{
	size_t width = CVPixelBufferGetWidth(pb), height = CVPixelBufferGetHeight(pb);

	widen_plane(CVPixelBufferGetBaseAddressOfPlane(from, 0), CVPixelBufferGetBytesPerRowOfPlane(from, 0),
	    CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), width, height, true);
	widen_plane(CVPixelBufferGetBaseAddressOfPlane(from, 1), CVPixelBufferGetBytesPerRowOfPlane(from, 1),
	    CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 1), (width + 1) / 2 * 2,
	    (height + 1) / 2, false);
}
