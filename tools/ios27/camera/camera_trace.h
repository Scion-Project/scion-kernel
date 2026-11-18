/* SPDX-License-Identifier: MIT-0 */
/* camera_trace.c: the camera's trace switch (cam_trace=N) and the vendor's methods traced */
#ifndef SCIONCAM_TRACE_H
#define SCIONCAM_TRACE_H

#define SCIONCAM_TRACE_FILE     "/private/var/hakios/camera/trace"

/* 0: off; N from cam_trace=N on the boot line, else from the file (1 when it holds no digit) */
int scioncam_trace_level(void);

/* wrap BWFigCaptureDeviceVendor's client, device and stream methods once its class is there (idempotent) */
void scioncam_trace_vendor(void);

/* cameracaptured: wrap the session server's application-state and display-layout methods (camera_statetrace.c) */
void scioncam_trace_state(void);

/* cameracaptured: wrap the graph's source node, its node outputs and the preview's sinks to say where frames go and stop
   (camera_flowtrace.c); scioncam_flow_emit is called by camera27.c for every sample a node output emits */
void scioncam_trace_flow(void);
void scioncam_flow_emit(void *output, void *sample_buffer);

/* cameracaptured, always (not the trace): a nil layout of the main display taken as the client in the foreground
   (camera_layout.c) */
void scioncam_layout_fallback(void);

/* cameracaptured, always (not the trace): sessions built without video thumbnails and the Live Photo movie (camera_refuse.c) */
void scioncam_no_thumbnails(void);

/* cameracaptured, always (not the trace): the stream's VideoOutputsColorInfo answered with the source node's own colour
   attachments, which its colour space check wants (camera_colour.c) */
void scioncam_colour_info(void);

/* SpringBoard, with the trace: wrap FBSDisplayLayoutPublisher's element, backlight, suppression and flush methods
   (camera_sbtrace.c) */
void scioncam_trace_layout_publisher(void);

/* Camera.app: wrap -[AVCaptureFigVideoDevice supportsAVCaptureSessionPreset:] to say the cameras' formats (camera_apptrace.c) */
void scioncam_trace_app(void);

/* camera_device.c: every call its device and streams get, said (0: as before) */
extern int hakios_camera_trace;

#endif
