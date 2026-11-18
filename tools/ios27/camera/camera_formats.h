/* SPDX-License-Identifier: MIT-0 */
/* camera_formats.c: the formats the S9's streams offer on iOS 27, as N104/AVCaptureSession.plist's stream formats */
#ifndef SCIONCAM_FORMATS_H
#define SCIONCAM_FORMATS_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>

/* camera_device.c's format(): a stream format of that sensor size, frames a second and field of view */
typedef CFDictionaryRef (*scioncam_format_fn)(int sensor_width, int sensor_height, int max_fps, bool binned, double fov);

/* the SupportedFormatsArray of the stream of the kernel's SENSOR (1 the back camera, 2 the front one); NULL for another */
CFArrayRef scioncam_n104_formats(uint32_t sensor, const char *camera, double fov, scioncam_format_fn make);

/* camera_tenbit.c: the still image output's 10-bit ('xf20') pool: an 8-bit 420f buffer of pb's size to draw into (NULL:
   none), and the picture widened from it into pb (both locked) */
#include <CoreVideo/CoreVideo.h>
CVPixelBufferRef scioncam_tenbit_scratch(CVPixelBufferRef pb);
void scioncam_tenbit_widen(CVPixelBufferRef from, CVPixelBufferRef pb);

/* camera_outputs.c: one line for each output a VideoOutputConfigurations / VideoOutputsEnabled / VideoOutputHandlers set
   names (its pixel format, size and pool), so that the console shows which outputs the graph uses */
void scioncam_say_outputs(const char *camera, CFStringRef key, CFDictionaryRef value);

#endif
