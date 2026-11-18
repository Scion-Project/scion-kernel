/*
 * SPDX-License-Identifier: MIT-0
 *
 * The flow trace's next step (cam_trace=1, with camera_flowtrace.c): where each node output's samples go, and whether the
 * node there takes them.
 *
 * Why: phone 1 with dylib -20 (18 November) had the graph running, the source node's Preview(PrimaryScaler) output
 * emitting about eight pictures a second, and the screen black: no node after the source node emitted anything, and
 * neither the preview stitcher nor the image queue sink camera_flowtrace.c wraps was given a sample. The preview's nodes
 * on iOS 27 are not the ones that file names.
 *
 * At the first sample a node output emits, scioncam_flow_next() says the node its connection leads to
 * (-[BWNodeOutput connection], -[BWNodeConnection input], -[BWNodeInput node], its class and -name), or that it has none,
 * and wraps that node class's own -renderSampleBuffer:forInput: and -handleDroppedSample:forInput: (each class once, at
 * most 24): their first call, then every 300th, is said with the input's name and the sample. A node that renders and
 * never emits is where the pictures stop; one that is never called is behind a connection that does not deliver.
 * An output that leads nowhere has the whole graph said once (-[BWGraph dotString], "trace: graph: ..." lines): phone 1 with
 * dylib -21 showed the source node's Preview(PrimaryScaler) output with no connection at all, so the preview's sink
 * pipeline is not attached to it, and the graph's own description shows what is there instead.
 * Names and types are iOS 27.0.1's CMCapture (24A446's shared cache: names only). NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
	snprintf(out, cap, "?");
	if (o != nil && CFGetTypeID((CFTypeRef)o) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)o, out, (CFIndex)cap, kCFStringEncodingUTF8);
	}
}

#define WRAPPED 24
static struct wrapped {
	Class c;
	IMP render, drop;
	unsigned long long renders, drops;
} wrapped[WRAPPED];
static unsigned nwrapped;
static pthread_mutex_t wrap_lock = PTHREAD_MUTEX_INITIALIZER;

/* the entry of self's class or the nearest wrapped superclass (the class whose own method was replaced) */
static struct wrapped *
entry_of(id self)
{
	for (Class c = object_getClass(self); c != Nil; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < nwrapped; i++) {
			if (wrapped[i].c == c) {
				return &wrapped[i];
			}
		}
	}
	return NULL;
}

static void
said(id self, struct wrapped *w, bool drop, id input)
{
	unsigned long long n = drop ? ++w->drops : ++w->renders;
	char in[48], node[64];

	if (n == 1 || n % 300 == 0) {
		name_of(call0(input, "name"), in, sizeof(in));
		name_of(call0(self, "name"), node, sizeof(node));
		say("trace: flow: %s \"%s\" %s on its input %s (%llu so far)", class_getName(object_getClass(self)), node,
		    drop ? "is given a DROPPED sample" : "renders a sample", in, n);
	}
}

static void
w_render(id self, SEL sel, void *sb, id input)
{
	struct wrapped *w = entry_of(self);

	if (w != NULL) {
		said(self, w, false, input);
		((void (*)(id, SEL, void *, id))w->render)(self, sel, sb, input);
	}
}

static void
w_drop(id self, SEL sel, id dropped, id input)
{
	struct wrapped *w = entry_of(self);

	if (w != NULL) {
		said(self, w, true, input);
		if (w->drop != NULL) {
			((void (*)(id, SEL, id, id))w->drop)(self, sel, dropped, input);
		}
	}
}

/* the class's own method of that name, or NULL */
static Method
own(Class c, SEL s)
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

static void
wrap_class(Class c)
{
	SEL rs = sel_registerName("renderSampleBuffer:forInput:"), ds = sel_registerName("handleDroppedSample:forInput:");
	Class k = c;
	Method rm = NULL, dm;

	/* the class in c's line that has its own render method (the one a call on c ends in) */
	while (k != Nil && (rm = own(k, rs)) == NULL) {
		k = class_getSuperclass(k);
	}
	if (rm == NULL || strcmp(class_getName(k), "BWNode") == 0 || method_getNumberOfArguments(rm) != 4) {
		say("trace: flow: %s renders with no method of its own: not traced", class_getName(c));
		return;
	}
	pthread_mutex_lock(&wrap_lock);
	for (unsigned i = 0; i < nwrapped; i++) {
		if (wrapped[i].c == k) {
			pthread_mutex_unlock(&wrap_lock);
			return;
		}
	}
	if (nwrapped == WRAPPED) {
		pthread_mutex_unlock(&wrap_lock);
		return;
	}
	struct wrapped *w = &wrapped[nwrapped];

	w->c = k;
	w->render = method_getImplementation(rm);
	dm = own(k, ds);
	w->drop = dm != NULL && method_getNumberOfArguments(dm) == 4 ? method_getImplementation(dm) : NULL;
	nwrapped++;
	method_setImplementation(rm, (IMP)w_render);
	if (w->drop != NULL) {
		method_setImplementation(dm, (IMP)w_drop);
	}
	pthread_mutex_unlock(&wrap_lock);
	say("trace: flow: %s's renderSampleBuffer:forInput: traced%s", class_getName(k), w->drop != NULL ? " (and its drops)" : "");
}

/* every output of a node and the node each leads to (phone 1, dylib -22: the graph gave no description) */
static void
say_outputs(id node)
{
	id outputs = call0(node, "outputs");
	CFIndex n = outputs != nil && CFGetTypeID((CFTypeRef)outputs) == CFArrayGetTypeID() ? CFArrayGetCount((CFArrayRef)outputs) : -1;
	char name[64], to[64];

	say("trace: graph: %s has %ld output(s); inputs %ld", node != nil ? class_getName(object_getClass(node)) : "nil", (long)n,
	    (long)(call0(node, "inputs") != nil ? CFArrayGetCount((CFArrayRef)call0(node, "inputs")) : -1));
	for (CFIndex i = 0; i < n && i < 40; i++) {
		id o = (id)CFArrayGetValueAtIndex((CFArrayRef)outputs, i), next = call0(call0(call0(o, "connection"), "input"), "node");

		name_of(call0(o, "name"), name, sizeof(name));
		name_of(call0(next, "name"), to, sizeof(to));
		say("trace: graph: output %ld %s -> %s%s%s", (long)i, name, next != nil ? class_getName(object_getClass(next)) : "nothing",
		    next != nil ? " " : "", next != nil ? to : "");
	}
}

/* the graph's own picture of itself (-[BWGraph dotString]), one console line per line of it, once a process */
static void
say_graph(id output)
{
	static int done;
	id graph = call0(call0(output, "node"), "graph"), dot = call0(graph, "dotString");
	CFIndex n;
	char *text;

	if (__atomic_fetch_add(&done, 1, __ATOMIC_RELAXED) != 0) {
		return;
	}
	say_outputs(call0(output, "node"));
	if (dot == nil || CFGetTypeID((CFTypeRef)dot) != CFStringGetTypeID()) {
		say("trace: graph: no description (graph %s, dotString %s)", graph != nil ? class_getName(object_getClass(graph)) : "nil",
		    dot != nil ? "not a string" : "nil");
		return;
	}
	n = CFStringGetMaximumSizeForEncoding(CFStringGetLength((CFStringRef)dot), kCFStringEncodingUTF8) + 1;
	if ((text = malloc((size_t)n)) == NULL || !CFStringGetCString((CFStringRef)dot, text, n, kCFStringEncodingUTF8)) {
		free(text);
		return;
	}
	unsigned lines = 0;

	for (char *last = NULL, *l = strtok_r(text, "\n", &last); l != NULL && lines < 400; l = strtok_r(NULL, "\n", &last)) {
		say("trace: graph: %.400s", l);
		lines++;
	}
	free(text);
}

/* called once for each node output, at its first emitted sample (camera_flowtrace.c) */
void
scioncam_flow_next(void *output_object, const char *node_class, const char *output_name)
{
	id output = (id)output_object, connection = call0(output, "connection"), input = call0(connection, "input");
	id node = call0(input, "node");
	char in[48], name[64];

	if (node == nil) {
		say("trace: flow: %s's output %s leads nowhere (connection %s, consumer %s)", node_class, output_name,
		    connection != nil ? "there" : "nil", call0(output, "consumer") != nil ? "there" : "nil");
		say_graph(output);
		return;
	}
	name_of(call0(input, "name"), in, sizeof(in));
	name_of(call0(node, "name"), name, sizeof(name));
	say("trace: flow: %s's output %s leads to %s \"%s\" (its input %s)", node_class, output_name,
	    class_getName(object_getClass(node)), name, in);
	wrap_class(object_getClass(node));
}
