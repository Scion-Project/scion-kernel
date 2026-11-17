/*
 * What the capture device (camera_device.c) says, and its trace: the lines go to mediaserverd's standard error (iOS 16.5),
 * or to the console (iOS 27's cameracaptured, HAKIOS_CAMERA_CONSOLE), prefixed "hakios: camera: ". Moved out of
 * camera_device.c on 17 November 2026, as they were, with the trace added.
 *
 * The trace (iOS 27's cam_trace=N: tools/ios27/camera/camera_trace.c sets hakios_camera_trace): every call the device and
 * its streams get, said with what was answered ("hakios: camera: trace: ..."): a name's first TRACE_EACH lines, then one
 * in 500 (some are read at every frame). Where the device or a stream is made, started, stopped or let go, the callers'
 * addresses too, the shared cache's slide taken off, to be looked up in the restore image's cache (docs/ios27/CAMERA.md,
 * "The trace"). Off (0: the default, and always on iOS 16.5): each property is said once, as before. NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "camera_say.h"

#define TRACE_EACH      40

int hakios_camera_trace;

void
hakios_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "hakios: camera: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
#ifdef HAKIOS_CAMERA_CONSOLE
	/*
	 * iOS 27 (tools/ios27/camera): cameracaptured's standard error goes nowhere, so the lines go to the console, which
	 * the kernel keeps in its log (as the status bar's c.dylib does in SpringBoard)
	 */
	static int console = -1;

	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	}
	if (console >= 0) {
		(void)write(console, line, (size_t)n);
		return;
	}
#endif
	write(2, line, (size_t)n);
}

/* ---- what is asked and not answered: said once per name (in the trace: every time) ------------------------------ */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

void
hakios_camera_say_value(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[300] = "";

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	hakios_camera_say("%s %s%s%s", what, name, value ? " = " : "", text);
}

void
hakios_camera_say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	if (hakios_camera_trace_named(what, NULL, key, value)) {
		return;
	}
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (first) {
		hakios_camera_say_value(what, key, value);
	}
}

/* ---- the trace ---------------------------------------------------------------------------------------------------- */

static CFMutableBagRef traced_names;

bool
hakios_camera_trace_named(const char *what, const char *camera, CFStringRef key, CFTypeRef value)
{
	CFStringRef tag;
	CFIndex count;
	char line[200];

	if (!hakios_camera_trace) {
		return false;
	}
	snprintf(line, sizeof(line), "trace: %s%s%s%s", what, camera != NULL ? " (" : "", camera != NULL ? camera : "",
	    camera != NULL ? ")" : "");
	tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), line, key);
	pthread_mutex_lock(&said_lock);
	if (traced_names == NULL) {
		traced_names = CFBagCreateMutable(NULL, 0, &kCFTypeBagCallBacks);
	}
	CFBagAddValue(traced_names, tag);
	count = CFBagGetCountOfValue(traced_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (count <= TRACE_EACH || count % 500 == 0) {
		if (count > TRACE_EACH) {
			strlcat(line, " (one in 500)", sizeof(line));
		}
		hakios_camera_say_value(line, key, value);
	}
	return true;
}

void
hakios_camera_trace_callers(const char *what, const char *camera)
{
	static intptr_t slide = -1;
	void *frames[16];
	char line[380];
	int n, len;

	if (!hakios_camera_trace) {
		return;
	}
	if (slide == -1) {
		slide = 0;
		for (uint32_t i = 0; i < _dyld_image_count(); i++) {
			const char *name = _dyld_get_image_name(i);

			if (name != NULL && strstr(name, "/CMCapture.framework/CMCapture") != NULL) {
				slide = _dyld_get_image_vmaddr_slide(i);
				break;
			}
		}
	}
	n = backtrace(frames, 16);
	len = snprintf(line, sizeof(line), "trace: %s%s%s%s: callers (unslid; slide 0x%lx):", what, camera != NULL ? " (" : "",
	    camera != NULL ? camera : "", camera != NULL ? ")" : "", (long)slide);
	for (int i = 2; i < n && len > 0 && len < (int)sizeof(line) - 16; i++) {
		uintptr_t at = (uintptr_t)frames[i] & 0x0000007fffffffffULL;

		len += snprintf(line + len, sizeof(line) - (size_t)len, " %lx", (unsigned long)(at - (uintptr_t)slide));
	}
	hakios_camera_say("%s", line);
}
