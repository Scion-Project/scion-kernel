/*
 * SPDX-License-Identifier: MIT-0
 *
 * Which of a stream's outputs iOS 27's capture graph configures, enables and gives handlers to, one console line each
 * (cam.dylib, called by camera_device.c for every VideoOutputConfigurations, VideoOutputsEnabled and VideoOutputHandlers
 * set; always on: a session sets them a handful of times).
 *
 * Why: the trace's "stream: set ... =" line gives CoreFoundation's description of the value, which runs over many lines,
 * and the console keeps only the first, so the outputs a graph used could not be read (phone 1, 18 November). Phone 1's
 * dylib -17 showed the graph configuring IntermediateTap and Depth, which camera_device.c did not keep, and then reading
 * VideoOutputConfigurations back empty and throwing ("output pixel format () doesn't match the pixelBufferPool's pixel
 * format (420f)", -[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:], 24A446). NOT RUN ON A PHONE.
 *
 * The read back. With IntermediateTap kept (dylib -18), phone 1 showed the graph's configurations: StillImage with its
 * PixelFormat (420f), PrimaryScaler (1504 x 1128) with none, and the same throw for PrimaryScaler. The source node
 * (24A446) sets a streaming output's PixelFormat from FigCapturePixelFormatWithLeastAverageBandwidth over
 * -[BWMultiStreamCameraSourceNode _supportedPixelFormatsForOutputID:] (a direct method: out of the runtime's reach), which
 * came back empty for it, while the output's pool, shared with the other streaming outputs (_shareStreamingBufferPools),
 * is 420f; allocateOrReuseBufferPoolsFromSourceNode: reads the configurations back from the stream and wants the two to
 * agree. scioncam_config_read_back() gives a configuration without a PixelFormat back with 420f in it, the format of the
 * pool and what camera_device.c draws (said once an output). Why the list came back empty is not known yet.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <string.h>
#include "camera_formats.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */

static void
fourcc(uint32_t v, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i) & 0xff);

		out[i] = c >= 32 && c < 127 ? c : '.';
	}
	out[4] = '\0';
}

static long
number(CFDictionaryRef d, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	long n = -1;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberLongType, &n);
	} else if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID()) {
		n = CFBooleanGetValue(v);
	}
	return n;
}

struct say_ctx {
	char line[900];
	int which;      /* 0 configurations, 1 enabled, 2 handlers */
};

static void
one(const void *key, const void *value, void *context)
{
	struct say_ctx *c = context;
	char name[48] = "?", more[160] = "";
	size_t used = strlen(c->line);

	if (CFGetTypeID(key) == CFStringGetTypeID()) {
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	}
	if (c->which == 1) {
		long on = -1;

		if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
			on = CFBooleanGetValue(value);
		} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberLongType, &on);
		}
		snprintf(more, sizeof(more), "=%ld", on);
	} else if (c->which == 0 && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		CFDictionaryRef d = value;
		CFTypeRef pool = CFDictionaryGetValue(d, CFSTR("BufferPool"));
		long pf = number(d, "PixelFormat");
		char f[5] = "----", pf4[5] = "----";
		size_t w = 0, h = 0;

		if (pool != NULL && CFGetTypeID(pool) == CVPixelBufferPoolGetTypeID()) {
			CFDictionaryRef a = CVPixelBufferPoolGetPixelBufferAttributes((CVPixelBufferPoolRef)pool);
			long t = a != NULL ? number(a, "PixelFormatType") : -1;

			w = a != NULL && number(a, "Width") > 0 ? (size_t)number(a, "Width") : 0;
			h = a != NULL && number(a, "Height") > 0 ? (size_t)number(a, "Height") : 0;
			if (t > 0) {
				fourcc((uint32_t)t, f);
			}
		}
		if (pf > 0) {
			fourcc((uint32_t)pf, pf4);
		}
		snprintf(more, sizeof(more), " (format %s, %ldx%ld, pool %s%s %zux%zu, %ld keys)", pf4, number(d, "Width"),
		    number(d, "Height"), pool != NULL ? "" : "NONE ", f, w, h, (long)CFDictionaryGetCount(d));
	}
	snprintf(c->line + used, sizeof(c->line) - used, "%s%s%s", used > 0 ? ", " : "", name, more);
}

void
scioncam_say_outputs(const char *camera, CFStringRef key, CFDictionaryRef value)
{
	struct say_ctx c = { "", 0 };
	char k[48] = "?";

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	c.which = strcmp(k, "VideoOutputConfigurations") == 0 ? 0 : strcmp(k, "VideoOutputsEnabled") == 0 ? 1 : 2;
	CFDictionaryApplyFunction(value, one, &c);
	hakios_camera_say("outputs: %s: %s: %s", camera, k, c.line[0] != '\0' ? c.line : "none");
}

CFDictionaryRef
scioncam_config_read_back(const char *camera, CFStringRef output, CFDictionaryRef config)
{
	static int said;
	int f420 = 0x34323066;  /* '420f' */
	char name[48] = "?";

	if (CFDictionaryGetValue(config, CFSTR("PixelFormat")) != NULL) {
		return CFRetain(config);
	}
	CFMutableDictionaryRef c = CFDictionaryCreateMutableCopy(NULL, 0, config);
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &f420);

	CFDictionarySetValue(c, CFSTR("PixelFormat"), n);
	CFRelease(n);
	if (said++ < 4) {
		CFStringGetCString(output, name, sizeof(name), kCFStringEncodingUTF8);
		hakios_camera_say("outputs: %s: %s's configuration has no PixelFormat: read back as 420f (its pool's)", camera, name);
	}
	return c;
}
