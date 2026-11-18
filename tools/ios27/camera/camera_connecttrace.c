/*
 * SPDX-License-Identifier: MIT-0
 *
 * How cameracaptured's graph connects its nodes, and which nodes it prepares (cam_trace=1, with the preview trace).
 *
 * Why: phone 1 with dylib -25 (18 November): the preview sink pipeline is made, from the camera source node, and still no
 * node is attached to any of the source node's preview outputs. In iOS 27.0.1's CMCapture (24A446's shared cache, its logic
 * only) -[BWGraph connectOutput:toInput:pipelineStage:deferredAttach:] makes a BWNodeConnection and attaches it at once, or,
 * with deferredAttach, later (-[BWNodeConnection deferredAttach], then attachNow when the nodes are prepared). An output
 * whose connection was made but never attached has no connection, as the preview output had.
 *
 * Said here (the first 80 of each): every connection asked for (the output's node and name, the input's node, deferred or
 * not), every deferred attach and every attachNow, and every node the graph prepares (-[BWGraph _prepareNode:cancellable:],
 * the node and what it returned). A preview connection asked for, deferred and never attached, with its sink's nodes never
 * prepared or failing to, names the step. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

static id
call0(id o, const char *sel)
{
	SEL s = sel_registerName(sel);

	return o == nil || !class_respondsToSelector(object_getClass(o), s) ? nil : ((id (*)(id, SEL))objc_msgSend)(o, s);
}

static void
name_of(id o, char *out, size_t cap)
{
	snprintf(out, cap, "-");
	if (o != nil && CFGetTypeID((CFTypeRef)o) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)o, out, (CFIndex)cap, kCFStringEncodingUTF8);
	}
}

/* "Class \"node name\".port" for a node output or input */
static void
port_of(id port, char *out, size_t cap)
{
	id node = call0(port, "node");
	char n[48], p[48];

	name_of(call0(node, "name"), n, sizeof(n));
	name_of(call0(port, "name"), p, sizeof(p));
	snprintf(out, cap, "%s \"%s\".%s", node != nil ? class_getName(object_getClass(node)) : "nil", n, p);
}

typedef bool (*connect_fn)(id, SEL, id, id, id, bool);
static connect_fn o_connect;
static void (*o_attach_now)(id, SEL);
static bool (*o_deferred)(id, SEL);     /* B16@0:8 on phone 1 (24B156) */
static long long (*o_prepare)(id, SEL, id, id);
static int n_connect, n_attach, n_deferred, n_prepare;

static bool
w_connect(id self, SEL sel, id output, id input, id stage, bool deferred)
{
	bool ok = o_connect(self, sel, output, input, stage, deferred);

	if (__atomic_fetch_add(&n_connect, 1, __ATOMIC_RELAXED) < 80) {
		char o[120], i[120];

		port_of(output, o, sizeof(o));
		port_of(input, i, sizeof(i));
		say("trace: connect: %s -> %s%s: %s", o, i, deferred ? " (deferred)" : "", ok ? "yes" : "NO");
	}
	return ok;
}

static void
said_connection(id self, const char *what, int *count)
{
	if (__atomic_fetch_add(count, 1, __ATOMIC_RELAXED) < 80) {
		char o[120], i[120];

		port_of(call0(self, "output"), o, sizeof(o));
		port_of(call0(self, "input"), i, sizeof(i));
		say("trace: connect: %s: %s -> %s", what, o, i);
	}
}

static void
w_attach_now(id self, SEL sel)
{
	said_connection(self, "attached", &n_attach);
	o_attach_now(self, sel);
}

static bool
w_deferred(id self, SEL sel)
{
	said_connection(self, "attach DEFERRED", &n_deferred);
	return o_deferred(self, sel);
}

static long long
w_prepare(id self, SEL sel, id node, id cancellable)
{
	long long r = o_prepare(self, sel, node, cancellable);

	if (__atomic_fetch_add(&n_prepare, 1, __ATOMIC_RELAXED) < 80) {
		char n[48];

		name_of(call0(node, "name"), n, sizeof(n));
		say("trace: connect: prepared %s \"%s\": %lld", node != nil ? class_getName(object_getClass(node)) : "nil", n, r);
	}
	return r;
}

/*
 * -[FigCapturePreviewSinkPipeline _buildVideoPreviewSinkPipeline:sourcePreviewOutput:sourceSensorRawPreviewOutput:
 * sourceHueMapPreviewOutput:graph:inferenceScheduler:captureDevice:previewTapDelegate:zoomPIPOverlayDelegate:
 * personalPhotographerCaptureDelegate:sourceStillImageOutputsByPortType:] (13 arguments, all objects; phone 1 with dylib
 * -26 on 24B156: the preview pipeline is made and asks for no connection at all): the output it is given and what it returns.
 */
typedef int (*build13)(id, SEL, id, id, id, id, id, id, id, id, id, id, id);
static build13 o_build;

static int
w_build(id self, SEL sel, id cfg, id preview, id raw, id hue, id graph, id inference, id device, id tap, id pip, id pp, id stills)
{
	char o[120];
	int r;

	port_of(preview, o, sizeof(o));
	r = o_build(self, sel, cfg, preview, raw, hue, graph, inference, device, tap, pip, pp, stills);
	say("trace: connect: the video preview sink pipeline is built from %s%s: %d", preview != nil ? o : "NO OUTPUT (nil)",
	    graph != nil ? "" : " with no graph", r);
	return r;
}

static bool
hook(const char *class_name, const char *selector, unsigned arguments, IMP with, IMP *original)
{
	Class c = objc_getClass(class_name);
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(selector)) : NULL;

	if (m == NULL || method_getNumberOfArguments(m) != arguments) {
		say("trace: connect: -[%s %s] is not as on iOS 27.0.1: not traced", class_name, selector);
		return false;
	}
	*original = method_getImplementation(m);
	method_setImplementation(m, with);
	say("trace: connect: -[%s %s] traced (%s)", class_name, selector, method_getTypeEncoding(m));
	return true;
}

void
scioncam_trace_connections(void)
{
	static bool done;
	unsigned n = 0;

	if (done || objc_getClass("BWGraph") == Nil) {
		return;
	}
	done = true;
	n += hook("BWGraph", "connectOutput:toInput:pipelineStage:deferredAttach:", 6, (IMP)w_connect, (IMP *)&o_connect);
	n += hook("BWNodeConnection", "attachNow", 2, (IMP)w_attach_now, (IMP *)&o_attach_now);
	n += hook("BWNodeConnection", "deferredAttach", 2, (IMP)w_deferred, (IMP *)&o_deferred);
	n += hook("BWGraph", "_prepareNode:cancellable:", 4, (IMP)w_prepare, (IMP *)&o_prepare);
	n += hook("FigCapturePreviewSinkPipeline", "_buildVideoPreviewSinkPipeline:sourcePreviewOutput:sourceSensorRawPreviewOutput:"
	    "sourceHueMapPreviewOutput:graph:inferenceScheduler:captureDevice:previewTapDelegate:zoomPIPOverlayDelegate:"
	    "personalPhotographerCaptureDelegate:sourceStillImageOutputsByPortType:", 13, (IMP)w_build, (IMP *)&o_build);
	say("trace: connect: %u of 5 methods traced", n);
}
