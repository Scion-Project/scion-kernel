/*
 * SPDX-License-Identifier: MIT-0
 *
 * SpringBoard's main-display layout, traced (cam.dylib in SpringBoard with cam_trace=1; docs/ios27/CAMERA.md, "Why the
 * display layout is empty"): why FrontBoard's main-display layout that SpringBoard publishes on this port names no
 * application and says backlightState 0 and backlightLevel 0 while the screen is on, which makes every client of it (the
 * capture session server among them) see no application in front (camera_layout.c stands in for it in cameracaptured).
 *
 * SpringBoard publishes the layout through FrontBoardServices' FBSDisplayLayoutPublisher: elements added and removed for
 * what is on the screen, the backlight's state and level set from the backlight's, publishing suppressed or held during
 * transitions by assertions with a reason, and flushed. Each of those calls is said here (the first 40 of each kind, then
 * every 200th), with its argument and, for the assertions, their reason, so that one boot shows which of them never comes
 * (no element added: SpringBoard's scene layout never reaches the publisher; a suppression that never ends; the backlight
 * never set). Each method is wrapped only if the class has it with the arguments of iOS 27.0.1's FrontBoardServices (24A446's
 * shared cache, read 18 November 2026: names only); one that differs is left alone and said. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
#define say hakios_camera_say

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

/* the first 40 calls of a kind, then every 200th */
static bool
worth(unsigned *n)
{
	unsigned k = __atomic_add_fetch(n, 1, __ATOMIC_RELAXED);

	return k <= 40 || k % 200 == 0;
}

static IMP o_add, o_remove, o_state, o_level, o_suppress, o_transition, o_end_suppression, o_end_transition, o_flush;
static unsigned n_add, n_remove, n_state, n_level, n_suppress, n_transition, n_end_suppression, n_end_transition, n_flush;

/* -_addElement:forKey: and -_removeElement:forKey:, where -addElement:, -add: and -remove: lead (void or an object
   returned: either is one register, passed back as it came) */
static id
w_add(id self, SEL sel, id element, id key)
{
	if (worth(&n_add)) {
		char t[300];

		text_of(element, t, sizeof(t));
		say("trace: layout publisher %p: add element (%u): %s", (void *)self, n_add, t);
	}
	return ((id (*)(id, SEL, id, id))o_add)(self, sel, element, key);
}

static id
w_remove(id self, SEL sel, id element, id key)
{
	if (worth(&n_remove)) {
		char t[300];

		text_of(element, t, sizeof(t));
		say("trace: layout publisher %p: remove element (%u): %s", (void *)self, n_remove, t);
	}
	return ((id (*)(id, SEL, id, id))o_remove)(self, sel, element, key);
}

static void
w_state(id self, SEL sel, long long state)
{
	if (worth(&n_state)) {
		say("trace: layout publisher %p: backlight state %lld (%u)", (void *)self, state, n_state);
	}
	((void (*)(id, SEL, long long))o_state)(self, sel, state);
}

static void
w_level(id self, SEL sel, double level)
{
	if (worth(&n_level)) {
		say("trace: layout publisher %p: backlight level %.3f (%u)", (void *)self, level, n_level);
	}
	((void (*)(id, SEL, double))o_level)(self, sel, level);
}

static id
w_suppress(id self, SEL sel, id reason)
{
	id r = ((id (*)(id, SEL, id))o_suppress)(self, sel, reason);

	if (worth(&n_suppress)) {
		char t[160];

		text_of(reason, t, sizeof(t));
		say("trace: layout publisher %p: layout SUPPRESSED for \"%s\" (%u): assertion %p", (void *)self, t, n_suppress, (void *)r);
	}
	return r;
}

static id
w_transition(id self, SEL sel, id reason)
{
	id r = ((id (*)(id, SEL, id))o_transition)(self, sel, reason);

	if (worth(&n_transition)) {
		char t[160];

		text_of(reason, t, sizeof(t));
		say("trace: layout publisher %p: transition \"%s\" (%u): assertion %p", (void *)self, t, n_transition, (void *)r);
	}
	return r;
}

static void
w_end_suppression(id self, SEL sel)
{
	if (worth(&n_end_suppression)) {
		say("trace: layout publisher %p: a suppression ends (%u)", (void *)self, n_end_suppression);
	}
	((void (*)(id, SEL))o_end_suppression)(self, sel);
}

static void
w_end_transition(id self, SEL sel)
{
	if (worth(&n_end_transition)) {
		say("trace: layout publisher %p: a transition ends (%u)", (void *)self, n_end_transition);
	}
	((void (*)(id, SEL))o_end_transition)(self, sel);
}

static void
w_flush(id self, SEL sel)
{
	((void (*)(id, SEL))o_flush)(self, sel);
	if (worth(&n_flush)) {
		char t[400];

		text_of(((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("currentLayout")), t, sizeof(t));
		say("trace: layout publisher %p: flushed (%u): %s", (void *)self, n_flush, t);
	}
}

static const struct wrap {
	const char *selector;
	unsigned arguments;
	char returns, argument;         /* the return type's ('*': void or an object) and the first argument's first characters
	                                   ('\0': none) */
	IMP wrapper, *original;
} wraps[] = {
	{ "_addElement:forKey:", 4, '*', '@', (IMP)w_add, &o_add },
	{ "_removeElement:forKey:", 4, '*', '@', (IMP)w_remove, &o_remove },
	{ "setBacklightState:", 3, 'v', 'q', (IMP)w_state, &o_state },
	{ "setBacklightLevel:", 3, 'v', 'd', (IMP)w_level, &o_level },
	{ "suppressLayoutForReason:", 3, '@', '@', (IMP)w_suppress, &o_suppress },
	{ "transitionAssertionWithReason:", 3, '@', '@', (IMP)w_transition, &o_transition },
	{ "_endSuppression", 2, 'v', '\0', (IMP)w_end_suppression, &o_end_suppression },
	{ "_endTransition", 2, 'v', '\0', (IMP)w_end_transition, &o_end_transition },
	{ "flush", 2, 'v', '\0', (IMP)w_flush, &o_flush },
};

void
scioncam_trace_layout_publisher(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class c;
	unsigned wrapped = 0;

	pthread_mutex_lock(&once_lock);
	if (done || (c = objc_getClass("FBSDisplayLayoutPublisher")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
		Method m = class_getInstanceMethod(c, sel_registerName(wraps[i].selector));
		char type[8] = "", argument[8] = "";

		if (m != NULL) {
			method_getReturnType(m, type, sizeof(type));
			if (method_getNumberOfArguments(m) > 2) {
				method_getArgumentType(m, 2, argument, sizeof(argument));
			}
		}
		/* (an integer of the backlight's state may be any width: 'q', 'Q', 'l', 'L', 'i' or 'I' are all one register) */
		bool integer = wraps[i].argument == 'q' && argument[0] != '\0' && strchr("qQlLiI", argument[0]) != NULL;

		bool returns = type[0] == wraps[i].returns || (wraps[i].returns == '*' && (type[0] == 'v' || type[0] == '@'));

		if (m == NULL || method_getNumberOfArguments(m) != wraps[i].arguments || !returns ||
		    (argument[0] != wraps[i].argument && !integer)) {
			say("trace: layout publisher: -[FBSDisplayLayoutPublisher %s] is not as on iOS 27.0.1 (%s): not traced",
			    wraps[i].selector, m == NULL ? "not there" : "other arguments or return type");
			continue;
		}
		*wraps[i].original = method_getImplementation(m);
		method_setImplementation(m, wraps[i].wrapper);
		wrapped++;
	}
	pthread_mutex_unlock(&once_lock);
	say("trace: layout publisher: %u of %zu methods traced (SpringBoard's main-display layout)", wrapped,
	    sizeof(wraps) / sizeof(wraps[0]));
}
