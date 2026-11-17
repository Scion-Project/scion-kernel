/*
 * SPDX-License-Identifier: MIT-0
 *
 * The preview's path through cameracaptured's capture graph, traced (cam_trace=1; docs/ios27/CAMERA.md, "Where the
 * pictures stop"): so that one boot with a black viewfinder names the node where the S9's frames stop.
 *
 * The camera's frames go from the stream's PixelBufferHandler blocks (camera_device.c) into the graph's source node
 * (BWMultiStreamCameraSourceNode), from node to node through -[BWNodeOutput emitSampleBuffer:], and to the screen through
 * the preview's image queue sink (BWImageQueueSinkNode, which hands IOSurfaces to Camera.app's preview layer). A node
 * that does not pass a frame on emits a dropped sample instead (-[BWNodeOutput emitDroppedSample:], its reason and time).
 * Said here, with the trace on:
 *   - which of the stream's outputs the source node configures, enables and gives handlers to (outputID and the
 *     configuration it hands the stream: its buffer pool, dimensions, pixel format);
 *   - the first sample each node output emits (node, output, size, pixel format, IOSurface or not, how far its time
 *     stamp is from the host clock's now), and every 10 seconds how many each has emitted;
 *   - every node's dropped samples (the first five of each node, then every 300th), with their reason;
 *   - the preview stitcher's and the image queue sink's first sample, the sink's image queue (nil: nothing to show), and
 *     the sink's own dropped samples.
 * Each method is wrapped only if the class itself has it with the number of arguments and return type of iOS 27.0.1's
 * CMCapture (24A446's shared cache, read 18 November 2026: names and types only); one that differs is left alone and said.
 * NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

static id
send0(id o, const char *sel)
{
	return o == nil ? nil : ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel));
}

static bool
sendb(id o, const char *sel)
{
	return o != nil && class_respondsToSelector(object_getClass(o), sel_registerName(sel)) &&
	    ((bool (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel));
}

/* an object's description, one line, at most cap - 1 characters */
static void
text_of(id o, char *out, size_t cap)
{
	CFStringRef d = o == nil ? NULL : CFCopyDescription((CFTypeRef)o);

	snprintf(out, cap, "nil");
	if (d != NULL) {
		CFStringGetCString(d, out, (CFIndex)cap, kCFStringEncodingUTF8);
		CFRelease(d);
	}
	for (char *c = out; *c; c++) {
		*c = *c == '\n' || *c == '\t' ? ' ' : *c;
	}
}

static void
string_of(id s, char *out, size_t cap)
{
	snprintf(out, cap, "?");
	if (s != nil && CFGetTypeID((CFTypeRef)s) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)s, out, (CFIndex)cap, kCFStringEncodingUTF8);
	}
}

/* a sample buffer's picture: "WxH 'fmt' IOSurface, time stamp N ms from now" */
static void
sample_of(void *sb, char *out, size_t cap)
{
	CVImageBufferRef ib = sb != NULL ? CMSampleBufferGetImageBuffer((CMSampleBufferRef)sb) : NULL;
	CMTime pts = sb != NULL ? CMSampleBufferGetPresentationTimeStamp((CMSampleBufferRef)sb) : kCMTimeInvalid;
	CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
	double ms = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(CMTimeSubtract(pts, now)) * 1000 : 0;

	if (ib == NULL || CFGetTypeID(ib) != CVPixelBufferGetTypeID()) {
		snprintf(out, cap, "no picture, time stamp %s%.1f ms from now", CMTIME_IS_VALID(pts) ? "" : "INVALID ", ms);
		return;
	}
	uint32_t pf = CVPixelBufferGetPixelFormatType(ib);

	snprintf(out, cap, "%zux%zu '%c%c%c%c' %s, time stamp %s%.1f ms from now", CVPixelBufferGetWidth(ib),
	    CVPixelBufferGetHeight(ib), pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255,
	    CVPixelBufferGetIOSurface(ib) != NULL ? "IOSurface" : "NO IOSurface", CMTIME_IS_VALID(pts) ? "" : "INVALID ", ms);
}

/* ---- every node output's samples, counted (called from camera27.c's -[BWNodeOutput emitSampleBuffer:]) ---------- */

#define FLOWS 64
static struct flow {
	Class node;
	char output[48];
	uint64_t emitted, dropped;
} flows[FLOWS];
static unsigned nflows;
static pthread_mutex_t flow_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_summary;
int scioncam_flow_on;

static struct flow *
flow_of(id output, bool *fresh)
{
	id node = send0(output, "node");
	Class c = node != nil ? object_getClass(node) : Nil;
	char name[48];

	string_of(send0(output, "name"), name, sizeof(name));
	*fresh = false;
	for (unsigned i = 0; i < nflows; i++) {
		if (flows[i].node == c && strcmp(flows[i].output, name) == 0) {
			return &flows[i];
		}
	}
	if (nflows == FLOWS) {
		return NULL;
	}
	flows[nflows].node = c;
	snprintf(flows[nflows].output, sizeof(flows[nflows].output), "%s", name);
	*fresh = true;
	return &flows[nflows++];
}

static void
summary(uint64_t now)
{
	static mach_timebase_info_data_t tb;
	char line[900];
	size_t n = 0;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	if (next_summary != 0 && now < next_summary) {
		return;
	}
	next_summary = now + 10ULL * 1000000000ULL * tb.denom / tb.numer;
	line[0] = '\0';
	for (unsigned i = 0; i < nflows && n < sizeof(line) - 80; i++) {
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s.%s %llu", i ? ", " : "",
		    flows[i].node ? class_getName(flows[i].node) + (strncmp(class_getName(flows[i].node), "BW", 2) == 0 ? 2 : 0) : "?",
		    flows[i].output, (unsigned long long)flows[i].emitted);
		if (flows[i].dropped) {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " (%llu dropped)", (unsigned long long)flows[i].dropped);
		}
	}
	say("trace: flow: emitted so far: %s", line);
}

void
scioncam_flow_emit(void *output_object, void *sample_buffer)
{
	id output = (id)output_object;
	bool fresh;
	struct flow *f;
	char what[200];

	if (!scioncam_flow_on) {
		return;
	}
	pthread_mutex_lock(&flow_lock);
	f = flow_of(output, &fresh);
	if (f != NULL) {
		f->emitted++;
	}
	if (fresh) {
		sample_of(sample_buffer, what, sizeof(what));
		say("trace: flow: %s's output %s emits its first sample: %s", f->node ? class_getName(f->node) : "?", f->output, what);
	}
	summary(mach_absolute_time());
	pthread_mutex_unlock(&flow_lock);
}

/* ---- the methods wrapped ------------------------------------------------------------------------------------------- */

static IMP o_dropped, o_iq_render, o_iq_drop, o_rq_render, o_stitch_render, o_out_config, o_add_handler, o_enable;

/* -[BWNodeOutput emitDroppedSample:] */
static void
w_dropped(id self, SEL sel, id dropped)
{
	if (scioncam_flow_on) {
		bool fresh;
		struct flow *f;
		char what[200];

		pthread_mutex_lock(&flow_lock);
		f = flow_of(self, &fresh);
		if (f != NULL && (f->dropped++ < 5 || f->dropped % 300 == 0)) {
			text_of(dropped, what, sizeof(what));
			say("trace: flow: %s's output %s DROPS a sample (%llu so far): %s", f->node ? class_getName(f->node) : "?",
			    f->output, (unsigned long long)f->dropped, what);
		}
		pthread_mutex_unlock(&flow_lock);
	}
	((void (*)(id, SEL, id))o_dropped)(self, sel, dropped);
}

static void
first_render(id self, const char *what, void *sb, uint64_t *count)
{
	char s[200];

	if ((*count)++ == 0) {
		sample_of(sb, s, sizeof(s));
		say("trace: flow: %s renders its first sample: %s", what, s);
	} else if (*count % 900 == 0) {
		say("trace: flow: %s has rendered %llu samples", what, (unsigned long long)*count);
	}
	(void)self;
}

/* -[BWImageQueueSinkNode renderSampleBuffer:forInput:]: the preview's last step in cameracaptured */
static void
w_iq_render(id self, SEL sel, void *sb, id input)
{
	static uint64_t count;
	bool first = count == 0;

	first_render(self, "the preview's image queue sink", sb, &count);
	((void (*)(id, SEL, void *, id))o_iq_render)(self, sel, sb, input);
	if (first) {
		char q[160];

		text_of(send0(self, "imageQueue"), q, sizeof(q));
		say("trace: flow: the image queue sink after its first sample: image queue %s, fences %s", q,
		    sendb(self, "fenceSupportEnabled") ? "on" : "off");
	}
}

/* -[BWImageQueueSinkNode handleDroppedSample:forInput:] */
static void
w_iq_drop(id self, SEL sel, id dropped, id input)
{
	static uint64_t count;

	if (count++ < 5 || count % 300 == 0) {
		char what[200];

		text_of(dropped, what, sizeof(what));
		say("trace: flow: the preview's image queue sink is given a DROPPED sample (%llu so far): %s",
		    (unsigned long long)count, what);
	}
	((void (*)(id, SEL, id, id))o_iq_drop)(self, sel, dropped, input);
}

/* -[BWRemoteQueueSinkNode renderSampleBuffer:forInput:] (video data and metadata outputs to the client) */
static void
w_rq_render(id self, SEL sel, void *sb, id input)
{
	static uint64_t count;

	first_render(self, "a remote queue sink", sb, &count);
	((void (*)(id, SEL, void *, id))o_rq_render)(self, sel, sb, input);
}

/* -[BWPreviewStitcherNode renderSampleBuffer:forInput:] */
static void
w_stitch_render(id self, SEL sel, void *sb, id input)
{
	static uint64_t count;

	if (count == 0) {
		say("trace: flow: the preview stitcher waits for a fenced update before its first emit: %s",
		    sendb(self, "waitsForFencedUpdateBeforeFirstEmit") ? "YES" : "no");
	}
	first_render(self, "the preview stitcher", sb, &count);
	((void (*)(id, SEL, void *, id))o_stitch_render)(self, sel, sb, input);
}

/* -[BWMultiStreamCameraSourceNode _outputConfigurationForStreamingOutputID:nodeOutput:] */
static id
w_out_config(id self, SEL sel, id output_id, id node_output)
{
	id config = ((id (*)(id, SEL, id, id))o_out_config)(self, sel, output_id, node_output);
	char name[48], node_name[48], c[700];

	string_of(output_id, name, sizeof(name));
	string_of(send0(node_output, "name"), node_name, sizeof(node_name));
	text_of(config, c, sizeof(c));
	say("trace: graph: the stream's output %s for the node output %s is configured: %s", name, node_name, c);
	return config;
}

/* -[BWMultiStreamCameraSourceNode _addPixelBufferHandler:videoOutputID:] */
static void
w_add_handler(id self, SEL sel, id handler, id output_id)
{
	char name[48];

	string_of(output_id, name, sizeof(name));
	say("trace: graph: a pixel buffer handler for the stream's output %s%s", name, handler == nil ? " (nil)" : "");
	((void (*)(id, SEL, id, id))o_add_handler)(self, sel, handler, output_id);
}

/* -[BWMultiStreamCameraSourceNode _setStreamOutputsEnabled:streamingOutputs:onDemandStillOutputs:] */
static void
w_enable(id self, SEL sel, bool on, id streaming, id stills)
{
	char a[300], b[160];

	text_of(streaming, a, sizeof(a));
	text_of(stills, b, sizeof(b));
	say("trace: graph: the stream's outputs %s: streaming %s, stills on demand %s", on ? "ENABLED" : "disabled", a, b);
	((void (*)(id, SEL, bool, id, id))o_enable)(self, sel, on, streaming, stills);
}

static const struct wrap {
	const char *class_name, *selector;
	unsigned arguments;
	char returns;
	IMP wrapper, *original;
} wraps[] = {
	{ "BWNodeOutput", "emitDroppedSample:", 3, 'v', (IMP)w_dropped, &o_dropped },
	{ "BWImageQueueSinkNode", "renderSampleBuffer:forInput:", 4, 'v', (IMP)w_iq_render, &o_iq_render },
	{ "BWImageQueueSinkNode", "handleDroppedSample:forInput:", 4, 'v', (IMP)w_iq_drop, &o_iq_drop },
	{ "BWRemoteQueueSinkNode", "renderSampleBuffer:forInput:", 4, 'v', (IMP)w_rq_render, &o_rq_render },
	{ "BWPreviewStitcherNode", "renderSampleBuffer:forInput:", 4, 'v', (IMP)w_stitch_render, &o_stitch_render },
	{ "BWMultiStreamCameraSourceNode", "_outputConfigurationForStreamingOutputID:nodeOutput:", 4, '@', (IMP)w_out_config,
	  &o_out_config },
	{ "BWMultiStreamCameraSourceNode", "_addPixelBufferHandler:videoOutputID:", 4, 'v', (IMP)w_add_handler, &o_add_handler },
	{ "BWMultiStreamCameraSourceNode", "_setStreamOutputsEnabled:streamingOutputs:onDemandStillOutputs:", 5, 'v',
	  (IMP)w_enable, &o_enable },
};

/* the class's own method of that name (not one it inherits: wrapping that would wrap every other subclass's too) */
static Method
own_method(Class c, SEL s)
{
	unsigned n = 0;
	Method *all = class_copyMethodList(c, &n), found = NULL;

	for (unsigned i = 0; all != NULL && i < n && found == NULL; i++) {
		if (method_getName(all[i]) == s) {
			found = all[i];
		}
	}
	free(all);
	return found;
}

void
scioncam_trace_flow(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done[sizeof(wraps) / sizeof(wraps[0])];
	unsigned wrapped = 0, tried = 0;

	pthread_mutex_lock(&once_lock);
	for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
		Class c;
		Method m;
		char type[8] = "";

		if (done[i]) {
			wrapped++;
			continue;
		}
		if ((c = objc_getClass(wraps[i].class_name)) == Nil) {
			continue;
		}
		done[i] = true;
		tried++;
		if ((m = own_method(c, sel_registerName(wraps[i].selector))) != NULL) {
			method_getReturnType(m, type, sizeof(type));
		}
		if (m == NULL || method_getNumberOfArguments(m) != wraps[i].arguments || type[0] != wraps[i].returns) {
			say("trace: flow: -[%s %s] is not as on iOS 27.0.1 (%s): not traced", wraps[i].class_name, wraps[i].selector,
			    m == NULL ? "not there" : "other arguments or return type");
			continue;
		}
		*wraps[i].original = method_getImplementation(m);
		method_setImplementation(m, wraps[i].wrapper);
		wrapped++;
	}
	scioncam_flow_on = 1;
	pthread_mutex_unlock(&once_lock);
	if (tried > 0) {
		say("trace: flow: %u of %zu methods traced", wrapped, sizeof(wraps) / sizeof(wraps[0]));
	}
}
