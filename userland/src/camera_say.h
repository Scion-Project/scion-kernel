/*
 * camera_say.c: what the capture device (camera_device.c) says, once per name, and its trace (cam_trace on iOS 27).
 */
#ifndef HAKIOS_CAMERA_SAY_H
#define HAKIOS_CAMERA_SAY_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void hakios_camera_say_value(const char *what, CFStringRef key, CFTypeRef value);
void hakios_camera_say_once(const char *what, CFStringRef key, CFTypeRef value);

/* the trace: 0 off (the default); else every call said (camera_say.c) */
extern int hakios_camera_trace;
/* in the trace, a name's line (CAMERA: the stream's camera, or NULL); false when the trace is off */
bool hakios_camera_trace_named(const char *what, const char *camera, CFStringRef key, CFTypeRef value);
/* in the trace, an event with its callers' unslid addresses */
void hakios_camera_trace_callers(const char *what, const char *camera);

#endif
