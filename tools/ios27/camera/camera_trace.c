/*
 * SPDX-License-Identifier: MIT-0
 *
 * The camera's trace in cameracaptured (docs/ios27/CAMERA.md, "The trace"): what the capture device vendor is asked and
 * answers, one line each, so that a phone boot names what stops a session that never asks for the camera's frames.
 *
 * The switch: cam_trace=1 on the boot image's command line (read from kern.bootargs), or the file
 * /private/var/hakios/camera/trace on the phone (its first character a digit, the level; empty or anything else: 1).
 * cam_trace=2 adds os_log's info and debug levels (camera_oslog.c). Off, nothing here runs and cam.dylib is as before.
 *
 * With it:
 *   - camera_device.c says every call its device and streams get, with the answer (hakios_camera_trace);
 *   - the program's os_log is taken (camera_oslog.c);
 *   - the vendor's methods below are wrapped: each says its arguments and what it gave back, then is the original. Each is
 *     checked by its number of arguments and its return type before it is wrapped (the names and types as iOS 27.0.1's
 *     CMCapture has them, read 17 November 2026 by class-dump of 24A446's shared cache: names and types only); one that
 *     differs is left alone and said.
 * The lines are "hakios: camera: trace: vendor: ...". NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/message.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "scion_bootline.h"
#include <unistd.h>
#include "camera_trace.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_device.c */
#define say hakios_camera_say

/* ---- the switch ------------------------------------------------------------------------------------------------- */

int
scioncam_trace_level(void)
{
	char file[8] = "";
	const char *value = scion_boot_arg("cam_trace", NULL);     /* the whole line: scion_bootline.h */
	int fd;

	if (value != NULL && atoi(value) > 0) {
		return atoi(value);
	}
	if ((fd = open(SCIONCAM_TRACE_FILE, O_RDONLY)) >= 0) {
		ssize_t got = read(fd, file, sizeof(file) - 1);

		close(fd);
		return got > 0 && file[0] >= '1' && file[0] <= '9' ? file[0] - '0' : 1;
	}
	return 0;
}

/* ---- how an argument is said ------------------------------------------------------------------------------------- */

static void
text_of(id object, char *out, size_t cap)
{
	if (object == nil) {
		snprintf(out, cap, "nil");
		return;
	}
	CFTypeID type = CFGetTypeID((CFTypeRef)object);

	if (type == CFStringGetTypeID() || type == CFArrayGetTypeID() || type == CFNumberGetTypeID() ||
	    type == CFBooleanGetTypeID() || type == CFDictionaryGetTypeID()) {
		CFStringRef d = CFCopyDescription((CFTypeRef)object);

		out[0] = '\0';
		if (d != NULL) {
			CFStringGetCString(d, out, (CFIndex)cap, kCFStringEncodingUTF8);
			CFRelease(d);
		}
		for (char *c = out; *c; c++) {
			*c = *c == '\n' || *c == '\t' ? ' ' : *c;
		}
		return;
	}
	snprintf(out, cap, "<%s %p>", object_getClassName(object), (void *)object);
}

#define TEXT(name, object) char name[160]; text_of((id)(object), name, sizeof(name))

/* ---- the vendor's methods, wrapped -------------------------------------------------------------------------------- */

static IMP o_register8, o_register7, o_unregister, o_copy_device, o_copy_device_id, o_copy_streams, o_take_back_device,
    o_take_back_streams, o_prewarm, o_invalidate_device;

/* -registerClientWithAuditToken:clientApplicationID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:
 *  deviceSharingWithOtherClientsAllowed:clientType:deviceAvailabilityChangedHandler: */
static int
w_register8(id self, SEL sel, audit_token_t token, id app, id description, int priority, BOOL steal, BOOL share, int type,
    id handler)
{
	int client = ((int (*)(id, SEL, audit_token_t, id, id, int, BOOL, BOOL, int, id))o_register8)(self, sel, token, app,
	    description, priority, steal, share, type, handler);
	TEXT(a, app);
	TEXT(d, description);

	say("trace: vendor: register client pid %d app %s (%s) priority %d steal %d share %d type %d -> client %d",
	    (int)token.val[5], a, d, priority, steal, share, type, client);
	return client;
}

/* -registerClientWithAuditToken:clientDescription:clientPriority:canStealFromClientsWithSamePriority:
 *  deviceSharingWithOtherClientsAllowed:clientType:deviceAvailabilityChangedHandler: */
static int
w_register7(id self, SEL sel, audit_token_t token, id description, int priority, BOOL steal, BOOL share, int type, id handler)
{
	int client = ((int (*)(id, SEL, audit_token_t, id, int, BOOL, BOOL, int, id))o_register7)(self, sel, token, description,
	    priority, steal, share, type, handler);
	TEXT(d, description);

	say("trace: vendor: register client pid %d (%s) priority %d steal %d share %d type %d -> client %d",
	    (int)token.val[5], d, priority, steal, share, type, client);
	return client;
}

/* -unregisterClient: */
static int
w_unregister(id self, SEL sel, int client)
{
	int r = ((int (*)(id, SEL, int))o_unregister)(self, sel, client);

	say("trace: vendor: unregister client %d -> %d", client, r);
	return r;
}

/* -copyDeviceForClient:informClientWhenDeviceAvailableAgain:error: */
static id
w_copy_device(id self, SEL sel, int client, BOOL inform, int *error)
{
	int e = 0;
	id device = ((id (*)(id, SEL, int, BOOL, int *))o_copy_device)(self, sel, client, inform, error != NULL ? error : &e);
	TEXT(dv, device);

	say("trace: vendor: copy device for client %d (inform %d) -> %s, error %d", client, inform, dv, error != NULL ? *error : e);
	return device;
}

/* -copyDeviceWithID:forClient:informClientWhenDeviceAvailableAgain:error: */
static id
w_copy_device_id(id self, SEL sel, id identifier, int client, BOOL inform, int *error)
{
	int e = 0;
	id device = ((id (*)(id, SEL, id, int, BOOL, int *))o_copy_device_id)(self, sel, identifier, client, inform,
	    error != NULL ? error : &e);
	TEXT(i, identifier);
	TEXT(dv, device);

	say("trace: vendor: copy device %s for client %d (inform %d) -> %s, error %d", i, client, inform, dv,
	    error != NULL ? *error : e);
	return device;
}

/* -copyStreamsFromDevice:positions:deviceTypes:deviceClientPriority:allowsStreamControlLoss:error: */
static id
w_copy_streams(id self, SEL sel, id device, id positions, id types, int priority, BOOL loss, int *error)
{
	int e = 0;
	id streams = ((id (*)(id, SEL, id, id, id, int, BOOL, int *))o_copy_streams)(self, sel, device, positions, types, priority,
	    loss, error != NULL ? error : &e);
	TEXT(p, positions);
	TEXT(t, types);

	say("trace: vendor: copy streams positions %s types %s priority %d loss %d -> %ld stream(s), error %d", p, t, priority, loss,
	    streams != nil && CFGetTypeID((CFTypeRef)streams) == CFArrayGetTypeID() ? (long)CFArrayGetCount((CFArrayRef)streams) :
	    (streams != nil ? 1L : 0L), error != NULL ? *error : e);
	return streams;
}

/* -takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:prefersDeviceInvalidatedImmediately: */
static void
w_take_back_device(id self, SEL sel, id device, int client, BOOL inform, BOOL now)
{
	TEXT(dv, device);

	say("trace: vendor: device %s given back by client %d (inform %d, invalidate now %d)", dv, client, inform, now);
	((void (*)(id, SEL, id, int, BOOL, BOOL))o_take_back_device)(self, sel, device, client, inform, now);
}

/* -takeBackStreams:device:totalStreamingDuration: */
static void
w_take_back_streams(id self, SEL sel, id streams, id device, unsigned duration)
{
	say("trace: vendor: %ld stream(s) given back after %u", streams != nil && CFGetTypeID((CFTypeRef)streams) ==
	    CFArrayGetTypeID() ? (long)CFArrayGetCount((CFArrayRef)streams) : -1L, duration);
	((void (*)(id, SEL, id, id, unsigned))o_take_back_streams)(self, sel, streams, device, duration);
}

/* -prewarmDefaultVideoDeviceForPID:completionHandler: */
static void
w_prewarm(id self, SEL sel, int pid, id handler)
{
	say("trace: vendor: prewarm the default device for pid %d", pid);
	((void (*)(id, SEL, int, id))o_prewarm)(self, sel, pid, handler);
}

/* -invalidateVideoDevice:forPID: */
static void
w_invalidate_device(id self, SEL sel, id device, int pid)
{
	TEXT(dv, device);

	say("trace: vendor: invalidate device %s for pid %d", dv, pid);
	((void (*)(id, SEL, id, int))o_invalidate_device)(self, sel, device, pid);
}

static const struct wrap {
	const char *selector;
	unsigned arguments;     /* method_getNumberOfArguments(): self and _cmd included */
	char returns;
	IMP wrapper, *original;
} wraps[] = {
	{ "registerClientWithAuditToken:clientApplicationID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
	  "deviceSharingWithOtherClientsAllowed:clientType:deviceAvailabilityChangedHandler:", 10, 'i', (IMP)w_register8, &o_register8 },
	{ "registerClientWithAuditToken:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
	  "deviceSharingWithOtherClientsAllowed:clientType:deviceAvailabilityChangedHandler:", 9, 'i', (IMP)w_register7, &o_register7 },
	{ "unregisterClient:", 3, 'i', (IMP)w_unregister, &o_unregister },
	{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", 5, '@', (IMP)w_copy_device, &o_copy_device },
	{ "copyDeviceWithID:forClient:informClientWhenDeviceAvailableAgain:error:", 6, '@', (IMP)w_copy_device_id, &o_copy_device_id },
	{ "copyStreamsFromDevice:positions:deviceTypes:deviceClientPriority:allowsStreamControlLoss:error:", 8, '@', (IMP)w_copy_streams,
	  &o_copy_streams },
	{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:prefersDeviceInvalidatedImmediately:", 6, 'v',
	  (IMP)w_take_back_device, &o_take_back_device },
	{ "takeBackStreams:device:totalStreamingDuration:", 5, 'v', (IMP)w_take_back_streams, &o_take_back_streams },
	{ "prewarmDefaultVideoDeviceForPID:completionHandler:", 4, 'v', (IMP)w_prewarm, &o_prewarm },
	{ "invalidateVideoDevice:forPID:", 4, 'v', (IMP)w_invalidate_device, &o_invalidate_device },
};

void
scioncam_trace_vendor(void)
{
	static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class vendor;
	unsigned wrapped = 0;

	pthread_mutex_lock(&once_lock);
	if (done || (vendor = objc_getClass("BWFigCaptureDeviceVendor")) == Nil) {
		pthread_mutex_unlock(&once_lock);
		return;
	}
	done = true;
	for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
		Method m = class_getInstanceMethod(vendor, sel_registerName(wraps[i].selector));
		char type[8] = "";

		if (m != NULL) {
			method_getReturnType(m, type, sizeof(type));
		}
		if (m == NULL || method_getNumberOfArguments(m) != wraps[i].arguments || type[0] != wraps[i].returns) {
			say("trace: vendor: -%s is not as on iOS 27.0.1 (%s): not traced", wraps[i].selector,
			    m == NULL ? "not there" : "other arguments or return type");
			continue;
		}
		*wraps[i].original = method_getImplementation(m);
		method_setImplementation(m, wraps[i].wrapper);
		wrapped++;
	}
	pthread_mutex_unlock(&once_lock);
	say("trace: vendor: %u of %zu methods traced", wrapped, sizeof(wraps) / sizeof(wraps[0]));
}
