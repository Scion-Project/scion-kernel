/*
 * SPDX-License-Identifier: MIT-0
 *
 * What the preview's pictures hold, traced (cam_trace=1; docs/ios27/CAMERA.md, "What the pictures hold"): so that one boot
 * with a black viewfinder names the first step whose picture is black, not only the steps that pass a sample on.
 *
 * Phone 1's last traced boot (18 November, dylib -30, cam_metal=1) had every node of the preview emit its samples, the
 * image queue sink hand 828 x 1702 '420f' IOSurfaces to its image queue and backboardd make a YCBCR8_420_2P (500) texture
 * over one of them, and the viewfinder pure black. The flow trace counts samples; nothing said what they held. Here, for the
 * pictures of the preview's path (each node output's 1st, 30th and 300th sample, and the image queue sink's), the processor
 * reads the picture through CoreVideo's read lock and says, from a 16 x 16 grid of luma and an 8 x 8 grid of chroma:
 * the luma's mean, least and most, the share of the grid above black, and the chroma's means (128 is grey: a black 420f
 * picture is Y 0 or 16, Cb Cr 128; a picture of zeros is Y 0, Cb Cr 0, which the screen would show green). It reads them
 * twice: as the processor's cache has them, and again after cleaning and invalidating the picture's lines
 * (sys_dcache_flush), so a picture that the GPU wrote and the processor's cache still hides is said as such ("after the
 * cache's clean: ..."). The IOSurface's ID is said, so that backboardd's line about the same surface (StarbiteMetal's
 * "ycbcr texture ... over surface N") can be put beside it.
 *
 * Reading is all it does: nothing is written, no picture is held past the call. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
void sys_dcache_flush(void *start, size_t length);                                           /* libSystem */

struct grid {
	unsigned mean, least, most, lit;        /* luma (or for BGRA, the mean of R, G and B); lit: of 256 points, above 24 */
	unsigned cb, cr;                        /* chroma means (BGRA: 0) */
};

static void
read_grid(CVPixelBufferRef pb, uint32_t pf, struct grid *g)
{
	size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
	unsigned long sum = 0, n = 0;

	memset(g, 0, sizeof(*g));
	g->least = 255;
	if (pf == kCVPixelFormatType_32BGRA) {
		const uint8_t *base = CVPixelBufferGetBaseAddress(pb);
		size_t row = CVPixelBufferGetBytesPerRow(pb);

		for (unsigned y = 0; base != NULL && y < 16; y++) {
			for (unsigned x = 0; x < 16; x++) {
				const uint8_t *p = base + (h * (2 * y + 1) / 32) * row + (w * (2 * x + 1) / 32) * 4;
				unsigned v = ((unsigned)p[0] + p[1] + p[2]) / 3;

				sum += v, n++;
				g->least = v < g->least ? v : g->least;
				g->most = v > g->most ? v : g->most;
				g->lit += v > 24;
			}
		}
		g->mean = n ? (unsigned)(sum / n) : 0;
		return;
	}
	const uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *chroma = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t lrow = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), crow = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1), ch = CVPixelBufferGetHeightOfPlane(pb, 1);
	unsigned long cbs = 0, crs = 0, cn = 0;

	for (unsigned y = 0; luma != NULL && y < 16; y++) {
		for (unsigned x = 0; x < 16; x++) {
			unsigned v = luma[(h * (2 * y + 1) / 32) * lrow + w * (2 * x + 1) / 32];

			sum += v, n++;
			g->least = v < g->least ? v : g->least;
			g->most = v > g->most ? v : g->most;
			g->lit += v > 24;
		}
	}
	for (unsigned y = 0; chroma != NULL && y < 8; y++) {
		for (unsigned x = 0; x < 8; x++) {
			const uint8_t *p = chroma + (ch * (2 * y + 1) / 16) * crow + (cw * (2 * x + 1) / 16) * 2;

			cbs += p[0], crs += p[1], cn++;
		}
	}
	g->mean = n ? (unsigned)(sum / n) : 0;
	g->cb = cn ? (unsigned)(cbs / cn) : 0;
	g->cr = cn ? (unsigned)(crs / cn) : 0;
}

static void
grid_text(const struct grid *g, bool bgra, char *out, size_t cap)
{
	if (bgra) {
		snprintf(out, cap, "RGB mean %u least %u most %u, %u of 256 above black", g->mean, g->least, g->most, g->lit);
	} else {
		snprintf(out, cap, "Y mean %u least %u most %u, %u of 256 above black; Cb %u Cr %u", g->mean, g->least, g->most,
		    g->lit, g->cb, g->cr);
	}
}

/* the picture of `sample_buffer`, said with `where` (for example "BWPreviewStitcherNode's output ?, sample 30") */
void
scioncam_pixels(const char *where, void *sample_buffer)
{
	scioncam_pixels_of(where, sample_buffer != NULL ? CMSampleBufferGetImageBuffer((CMSampleBufferRef)sample_buffer) : NULL);
}

/* the picture of a pixel buffer, said with `where` (camera_stitchtrace.c: a transfer's destination before and after) */
void
scioncam_pixels_of(const char *where, void *pixel_buffer)
{
	CVImageBufferRef ib = pixel_buffer;
	struct grid before, after;
	char a[160], b[160];

	if (ib == NULL || CFGetTypeID(ib) != CVPixelBufferGetTypeID()) {
		return;
	}
	CVPixelBufferRef pb = (CVPixelBufferRef)ib;
	uint32_t pf = CVPixelBufferGetPixelFormatType(pb);
	bool bgra = pf == kCVPixelFormatType_32BGRA;

	if (!bgra && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
		hakios_camera_say("trace: pixels: %s: '%c%c%c%c' is not read here", where, pf >> 24, pf >> 16 & 255, pf >> 8 & 255,
		    pf & 255);
		return;
	}
	if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		hakios_camera_say("trace: pixels: %s: the picture could not be locked for reading", where);
		return;
	}
	IOSurfaceRef surface = CVPixelBufferGetIOSurface(pb);

	read_grid(pb, pf, &before);
	/* the picture's lines cleaned and invalidated: what the GPU wrote, where the processor's cache held older lines */
	if (CVPixelBufferIsPlanar(pb)) {
		for (size_t p = 0; p < CVPixelBufferGetPlaneCount(pb); p++) {
			void *base = CVPixelBufferGetBaseAddressOfPlane(pb, p);

			if (base != NULL) {
				sys_dcache_flush(base, CVPixelBufferGetBytesPerRowOfPlane(pb, p) * CVPixelBufferGetHeightOfPlane(pb, p));
			}
		}
	} else if (CVPixelBufferGetBaseAddress(pb) != NULL) {
		sys_dcache_flush(CVPixelBufferGetBaseAddress(pb), CVPixelBufferGetBytesPerRow(pb) * CVPixelBufferGetHeight(pb));
	}
	read_grid(pb, pf, &after);
	CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	grid_text(&before, bgra, a, sizeof(a));
	if (memcmp(&before, &after, sizeof(before)) == 0) {
		hakios_camera_say("trace: pixels: %s: %zux%zu '%c%c%c%c' surface %u: %s", where, CVPixelBufferGetWidth(pb),
		    CVPixelBufferGetHeight(pb), pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255,
		    surface != NULL ? IOSurfaceGetID(surface) : 0, a);
	} else {
		grid_text(&after, bgra, b, sizeof(b));
		hakios_camera_say("trace: pixels: %s: %zux%zu '%c%c%c%c' surface %u: %s; after the cache's clean: %s", where,
		    CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255,
		    surface != NULL ? IOSurfaceGetID(surface) : 0, a, b);
	}
}

/* whether the `count`th sample (from 1) of a picture-carrying output is one to read: the 1st, 30th, 300th, then every 3000th */
bool
scioncam_pixels_due(unsigned long long count)
{
	return count == 1 || count == 30 || count == 300 || (count > 300 && count % 3000 == 0);
}
