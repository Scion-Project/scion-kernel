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

/* cameracaptured, always (not the trace): a nil layout of the main display taken as the client in the foreground
   (camera_layout.c) */
void scioncam_layout_fallback(void);

/* cameracaptured, always (not the trace): sessions built without video thumbnails and the Live Photo movie (camera_refuse.c) */
void scioncam_no_thumbnails(void);

/* Camera.app: wrap -[AVCaptureFigVideoDevice supportsAVCaptureSessionPreset:] to say the cameras' formats (camera_apptrace.c) */
void scioncam_trace_app(void);

/* camera_device.c: every call its device and streams get, said (0: as before) */
extern int hakios_camera_trace;

#endif
