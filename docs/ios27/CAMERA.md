<!-- SPDX-License-Identifier: MIT-0 -->
# The camera on iOS 27 (Galaxy S9)

State, 17 November 2026: **built, not run on a phone.** The kernel's camera driver has run on the S9 under iOS 27's kernel
(the rear camera's stream, 12 November; scion-starlte `CAMERA.md`), and the capture device below streamed the S9's front
camera into the stock Camera app on iOS 16.5 (3 November). The iOS 27 pieces on this page (the library in cameracaptured,
the root step, the device-tree node) have been built and checked on a copy of a root; no phone has booted them. The phone
test at the end is what is left.

What it gives when it works: the Camera app on the home screen, its preview from the S9's camera, and a photograph. One
camera per boot: the front one (`cam_s9=7`), or the rear one (`cam_s9=7 cam_rear=1`). The app's back and front cameras
both show the camera of that boot (below, "One camera per boot").

## Turning it on

1. The root: step `94-camera` (`tools/ios27/fullroot/steps.d/94-camera.sh`, on by default, `ENABLE_CAMERA=0` leaves it out)
   builds `/usr/lib/cam.dylib` with your Xcode and records it for cameracaptured. Optional: `CAMERA_TUNING_DIR`, colour
   tunings `camera-<sensor>.isp` made from your own phone's files (`tools/camtuning-exynos.py`); without them the pictures
   have no colour matrix and no lens shading (right, and flat).
2. The boot image, made again after the root, with the kernel's camera asked for:

       tools/ios27/s9/make-boot-image.sh --kernel hakios-kernel --donor-adt inputs/n104ap.adt --template inputs/recovery.img \
           --cmdline 'sbgl=0 sweetspeed=408782847 cam_s9=7'              # the rear camera: ... cam_s9=7 cam_rear=1

   With `cam_s9=7` on the line and the record naming `cam.dylib`, the device tree gets `/arm-io/isp` and iOS shows the
   Camera app (`--camera-node auto`, the default; `yes` or `no` to force it). Without either, the tree is as before and the
   app stays hidden. `cam_s9` is not one of the kernel's built-in arguments: whether it becomes one is Claudius's decision.

Nothing here writes to the camera's lens memory, and nothing touches the iris camera or its IR LED.

## How iOS 16.5 does it, and how iOS 27 differs

| | iOS 16.5 (`userland/`, the Redwood root) | iOS 27 (this page) |
|---|---|---|
| Sensor to memory | `hakios.exynos_camera.c`, `cam_s9=7`: the S5K3H1 on CSIS1, RAW10 1632 x 1224 at 30 frames a second into a ring of four | the same file (`profiles/ios-27.copy`), the same argument; `cam_rear=1`: the S5K2L3 on CSIS0, 2016 x 1512 |
| To user space | `/dev/hakios-camera` (`userland/src/hakios.camera.h`: a `RWFRAME1` header, then the frame; commands EXPOSURE, SENSOR, FOCUS) | the same node |
| Who reads it | mediaserverd, the capture stack's host: `libhakios.media.dylib` (`inject_media.c` + `camera_device.c` + `camera_isp.c`), inserted by the kernel | **cameracaptured** (`/usr/libexec/cameracaptured`, user mobile): iOS 27 has no mediaserverd. `cam.dylib` (`tools/ios27/camera/camera27.c` + the same `camera_device.c` and `camera_isp.c`), inserted by the kernel (`sweetinject=` from the root build's record) |
| How the capture stack gets the device | `+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]` replaced: with a camera, a vendor made by `-initWithDeviceCreateFunction:` with `hakios_camera_device_create` | the vendor is made by `+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:]`, which, given no function, takes `FigCaptureGetCaptureDeviceCreateFunction()`'s: Apple's camera driver found in IOKit (`AppleH10CamIn` on the iPhone 11) and the ISP plug-in's create function (`H10ISPCaptureDeviceCreate`, `/System/Library/MediaCapture/H10ISP.mediacapture`, in the shared cache). The S9 has neither: no device. `cam.dylib` gives that class method `hakios_camera_device_create` when it is given none |
| The device's interface | CoreMedia derived objects: base class v1; device class v3 (ProcessingSessionCreate, RequestStreams, RelinquishStreams, PerformPearlCalibration); stream class v2 (Start, Stop, AddAttachmentsToSampleBuffer); properties by name; frames to `VideoOutputHandlers`' `PixelBufferHandler` blocks | **the same shapes** in iOS 27.0.1's `kH10ISPCaptureDeviceVTable` and `kH10ISPCaptureStreamVTable`, the same create function's arguments (allocator, options, out); every property name `camera_device.c` uses is a string of iOS 27's CMCaptureCore, CMCaptureDevice or CMCapture (one, `UnlockAWBNow`, is not: it is answered if asked, and nothing asks it) |
| Formats | named as the iPhone X's (`D22/AVCaptureSession.plist`: back "Photo" 4032 x 3024, "Video-60" 4224 x 2376; front 3088 x 2320, 3840 x 2160) | iOS 27's CMCapture has only the iPhone 11's `N104/AVCaptureSession.plist`: back "Photo" 4224 x 3168 at 60, "Video-60" 4224 x 2384; front "Photo" 4032 x 3024, "Video-30" 4032 x 2272 (`HAKIOS_CAMERA_N104` in `camera_device.c`) |
| Refusals of hardware the phone lacks | portrait prewarm skipped, no-audio 'nope' taken as started, video stabilization 0, quality prioritization at most 1, GNR passed by, NULL buffers not emitted, VCPHEVC's software HEVC encoder registered; each checked against an address of 20F66's shared cache | the same methods, the same names and arguments in iOS 27.0.1, checked by their number of arguments and return type (addresses of 16.5 mean nothing here); the Metal device's removal and the flashlight's own vendor (16.5 addresses) are not carried over |
| Its log | mediaserverd's standard error | the console (the kernel's log): `hakios: camera: ...` (the device) and `SCIONCAM[pid]: ...` (the library) |
| Why the Camera app is on the home screen | `Camera.app`'s `SBAppTags` = `still-camera`; MobileGestalt answers it from `IODeviceTree:/arm-io/isp` (`camera-rear`), which the loader made with `cam=5` | the same tag; iOS 27's libMobileGestalt answers `still-camera` from `/arm-io/isp`'s `camera-front` or `camera-rear`, `front-facing-camera` from `camera-front`, `rear-facing-camera` from `camera-rear`. The S9's iOS 27 tree has no isp node, so the app is hidden. `make-boot-image.sh` now adds it (`tools/ios27/camera/adt-isp-node.py`), only for a boot that has the camera |
| A cached "no" | | MobileGestalt keeps answers in a cache on /var (`com.apple.MobileGestalt.plist` in its system group container): `94-camera` removes it from the root it builds so that it is made again from the next boot's tree |
| Strict mode | `/usr/sbin/mediaserverd` in the node's list | `/usr/libexec/cameracaptured` added to the list (`hakios_dev_strict=1`, `drivers/core/hakios.dev_strict.h`) |

All the iOS 27 facts above were read from iOS 27.0.1 (24A446)'s files on the build machine (launchd's job, cameracaptured's
imports, CMCapture's and libMobileGestalt's code by disassembly, the plist's format names and sizes): names and numbers only,
nothing of Apple's copied into this repository.

## Without the camera: what the phone does today

Every iOS 27 root of the project has `/Applications/Camera.app` (it is part of the iPhone 11's system). cameracaptured runs
from boot (launchd's job, the feature flag `CameraCapture/cameracaptured` on). The S9's device tree has no `/arm-io/isp`,
so MobileGestalt answers no camera and SpringBoard does not show the app's icon; the app can still be opened another way
(the Lock Screen's camera control, Control Center, a widget), and then it runs with no capture device: on 17 November a
phone in the test session had it in front with a black viewfinder after a touch, not crashed. Whether an older
MobileGestalt cache on a phone that has been booted many times says otherwise is not known (the cache was not read).

## One camera per boot

The S9's driver drives one sensor per boot: the front S5K3H1 unless `cam_rear=1`, then the rear S5K2L3. The node's frames
carry the sensor number last asked for (`HAKIOS_CAMERA_SENSOR` only relabels them on the S9: `asked_sensor` in
`hakios.exynos_camera.c`), so the device's back stream (sensor 1) and front stream (sensor 2) both get the boot's camera.
Switching between the two in one boot (the app's button) needs the driver to power one sensor down and the other up and to
move between CSIS1 and CSIS0 on that command, as the S9+'s driver does; that is not written. With it, nothing on the iOS
side changes: `camera_device.c` already asks for its stream's sensor when a stream starts.

## Not done

- Run on a phone: everything on this page.
- Switching cameras within a boot (above).
- The front camera's focus motor from iOS (16.5's `inject_media.c` gives the iPhone X plist's front camera the back camera's
  autofocus keys; N104's plist has the same layout, not done here).
- Photographs in HEIC need the software encoder registration to work in cameracaptured as it did in mediaserverd; if it
  does not, the app's Formats setting "Most Compatible" (JPEG) is the way round.

## The phone test

Boot arguments: the root with `94-camera` (check the build's line `94-camera: /usr/lib/cam.dylib built ...` and
`make-boot-image: ... adt-isp-node: /arm-io/isp with camera-front and camera-rear`), `--cmdline 'sbgl=0 sweetspeed=408782847
cam_s9=7'`. In the console log, in order:

| Stage | Look for | If not |
|---|---|---|
| kernel | `exynos_camera: 7: THE STREAM: ... up`, `exynos_camera: 7: /dev/hakios-camera made`; once a reader is there, `the stream: frame N ... frames a second` every ten seconds | the driver's own run table (scion-starlte `CAMERA.md`, C1-C7) |
| cameracaptured starts | `SCIONCAM[pid]: in cameracaptured: the kernel has a camera`, eight `... replaced` lines, `the software HEVC encoder ... registered` | no `SCIONCAM` at all: `sweetinject=` lacks `cam.dylib` (the record, `make-boot-image.sh`'s line) or dyld refused it. `... is not as on iOS 27.0.1 ... left alone`: another iOS build |
| SpringBoard | the Camera icon (the console's screenshot) | MobileGestalt's cache (was step 94 applied?), or the tree has no isp node (`make-boot-image`'s line) |
| the app opens (tap the icon, or swipe left on the Lock Screen) | `SCIONCAM: the capture device vendor is made with the S9's camera device`, `the capture device vendor: made`, `hakios: camera: the device made: two streams`, `hakios: camera: stream: frames begin`, `PrimaryScaler: the first picture given` | `vendor: NONE`: the stack refused the device; `hakios: camera: ... asked ...` lines name what the stack asked and was not answered |
| the preview | the console's screenshot shows the room | black with frames begun: the outputs' configuration; look for `hakios: camera:` lines about `VideoOutputConfigurations` |
| the shutter | `hakios: camera: stream: a photograph asked for ...`, the still (the StillImage output's `the first picture given` at the sensor's full size), `a photograph goes past the noise reduction`, then a new picture in Photos (open it: the console's screenshot) | no picture and no error: the encoder; set the app's Formats to Most Compatible (JPEG) and try again |
| the rear camera | boot again with `cam_s9=7 cam_rear=1`: the same lines, the picture from the back | |

A run that ends with the Camera app crashing or cameracaptured restarting (`exec cameracaptured` more than once): the
console's crash lines name the place; `ENABLE_CAMERA=0`, or a boot image without `cam_s9=7`, gives the phone back as it was.

## The first phone test (17 November) and the trace

Phone 1 booted this page's pieces on 17 November with the front camera (`cam_s9=7`). The kernel, cameracaptured and
SpringBoard rows of the table above passed. The capture stack also ran a session of its own about 9 seconds after
cameracaptured started, before any tap. It asked the device for its streams, and 30 frames of 1632 x 1224 came from the
kernel before it gave them back (`device: streams given back (2)`). The app's row failed: the Camera app opened in Photo
mode, with its flip button, but its viewfinder stayed black. What the console says around the tap, in order:

1. The touch goes down on the icon. `SWEET Sandbox: cameracaptured privilege 1003 denied`, then `hakios: camera: the
   device made: two streams` follow at once, still before `exec Camera`.
2. `exec Camera`.
3. Two seconds after the touch, `hakios: camera: stream: stop (0 frames, 0 pictures given)` comes twice: the two streams of
   the device made in step 1 are let go.
4. Nothing more of the camera's for the 35 seconds the log goes on. No `device made`, no `streams asked for`, no
   `frames begin`. Camera and cameracaptured do not crash.

What that settles, from the log and from iOS 27.0.1's CMCapture (24A446's shared cache, names and code read on the build
Mac):

- **The device in step 1 is SpringBoard's prewarm, not the app's session.** It is made on the touch-down, before the app
  exists. CMCapture's `FigCaptureSessionPrewarm` takes the vendor's default device ahead of a launch
  (`-[BWFigCaptureDeviceVendor prewarmDefaultVideoDeviceForPID:completionHandler:]`). A device no client takes is closed by
  the vendor's close timer (`_setupDeviceCloseTimerForDevice:reason:`): that is the two `stop` lines of step 3, the streams'
  invalidation, and not a refusal.
- **Privilege 1003 is `PRIV_SYSTEM_OVERRIDE`** (`bsd/sys/priv.h`). Its caller is `cs_prewarmSystemOverride`'s block in
  CMCapture: `system_override(1000000000, 4)` (4 is `SYS_OVERRIDE_FAST_JETSAM`) on a queue of its own, its result not
  looked at. Denied, the prewarm only goes without the faster jetsam; it is not what stops the camera. **1015 is
  `PRIV_PROC_CPUMON_OVERRIDE`** (the next line is the kernel's `denied attempt to escape CPU monitor`): cameracaptured
  stays under the default CPU monitor, which matters only to a process that uses the CPU for long. On an iPhone both are
  granted by entitlements cameracaptured has (`com.apple.private.kernel.system-override` and
  `com.apple.private.kernel.override-cpumon`). The fork's sandbox stand-in grants neither (`sweet_sandbox_priv_grant` in
  `bsd/kern/sweet_amfi.c` grants only `PRIV_WORK_INTERVAL` and `PRIV_DARKBOOT`). The fork's pull request 39 adds
  `sweet_priv_ent=1` (off by default): with it, a process that holds either entitlement is granted that privilege.
- **The app's session never asked cameracaptured for the camera.** A session that builds its graph needs the device again (the
  prewarm's was closed), and `the device made` is said at every creation. It also asks for streams, which is said for the
  first three requests (two were used at boot, so the third would have been said). Neither line came. So the stop is before
  the vendor: in the app, or in cameracaptured's session server before it builds a graph. It is not in what the device or
  its streams answer (`asked for ...`). The causes that the table above names for this row (`vendor: NONE`, unanswered
  asks) are not what happened.
- What is left: the app not starting its session (its own preflight: camera authorization, the device it is given, the
  state it waits for), or the session server not running it. The server's decision line has the form `ClientStartedSession:%d
  Cam/Audio:%d/%d bg:%d prewarming:%d ... shouldRun:%d`: whether the server thinks the app is in the background, and whether
  TCC refused the client (`FigCaptureTCCAccessCheckAuditToken`). Its arguments are not in the console because logd is off
  on this root and os_log goes nowhere. The trace below takes it.

### The trace

`cam.dylib` has a trace, off by default. It is on with `cam_trace=1` on the boot image's command line, or with the file
`/private/var/hakios/camera/trace` on the phone (`94-camera.sh` writes it with `CAMERA_TRACE=1`). With it:

- `hakios: camera: trace: ...`: every call the device and its streams get, with what was answered (a name's first 40,
  then one in 500). Where the device or a stream is made, started, stopped, invalidated or freed, its callers too, as
  shared-cache addresses with the slide taken off. Look them up with `ipsw dyld a2s CACHE ADDRESS` (the cache of the
  restore image, as `crashlog-symbolicate.py` does).
- `hakios: camera: trace: vendor: ...`: the capture device vendor's register client, copy device, copy streams, take
  back, prewarm and unregister calls, with the client's pid and application, the positions and types asked for, and the
  error each returned. The methods are wrapped only where their arguments and return type are as on iOS 27.0.1.
- `CAMLOG[pid cameracaptured] K: ...`: cameracaptured's own os_log (K: I default, E error, F fault, D debug). Errors
  and faults always go to the console; other lines go only when they name the camera's business (capture, session,
  device, stream, TCC, background, ...). At most 3000 lines go to the console, and every line goes to
  `/private/var/local/oslog/cameracaptured.log`. `cam_trace=2` also turns on os_log's info and debug levels.
- With `CAMERA_TRACE_APP=1` (or the root's inject record's line edited to `/usr/lib/cam.dylib cameracaptured Camera`), the
  library also goes into Camera.app. There it only takes the app's os_log (`CAMLOG[pid Camera] ...`) while the trace is
  on, and replaces nothing.

Without the switch the library does what it did before, and `camera_device.c` on iOS 16.5 is unchanged in behaviour.
**The trace has not been run on a phone.**

### The test that decides it

1. Root: this branch's `cam.dylib` (`tools/ios27/camera/build.sh`) in `/usr/lib/cam.dylib`. Better, the root step with
   `CAMERA_TRACE_APP=1`, or the record edited as above, and the boot image made again.
2. Boot line: as on 17 November, plus `cam_trace=1`: `--cmdline 'sbgl=0 sweetspeed=408782847 cam_s9=7 cam_trace=1'`.
3. Wait about 60 seconds after SpringBoard is up. Tap the Camera icon once. Wait 30 seconds. Take a screenshot.
   Optionally, tap the flip button and wait 10 seconds.
4. The lines expected, in order, and what a missing one means:

| Line | Missing means |
|---|---|
| `SCIONCAM[pid]: in cameracaptured: the trace is on (cam_trace=1) ... (4 of 4 entry points)` | the switch was not seen (boot line or file); fewer than 4: os_log is only partly taken |
| `trace: vendor: 10 of 10 methods traced` | a method is named `not as on iOS 27.0.1`: that call is not traced |
| at the tap: `trace: vendor: prewarm the default device for pid N`, `trace: device: made: callers ...` | SpringBoard did not prewarm (no harm) |
| `trace: vendor: register client pid P app com.apple.camera ...` (P is Camera's pid from `exec Camera`) | **the app never reached cameracaptured**: read the `CAMLOG[pid Camera]` lines (the app's own reason: authorization, the session's runtime error or interruption) |
| `trace: vendor: copy device for client C ... -> <...>, error 0` | an error number here, or `nil`: the vendor refused the session the device. The `CAMLOG[... cameracaptured]` lines just before it say why (TCC, priority, the client's state) |
| `trace: vendor: copy streams positions ... -> 2 stream(s), error 0` | the session wanted a camera the device does not offer (the positions and types are in the line) |
| `hakios: camera: device: streams asked for`, `stream: frames begin`, `PrimaryScaler: the first picture given` | the graph was built but did not start: the `trace: stream (...): copy, NOT answered` lines and the `CAMLOG` errors just before the stop name the property or the step |
| a `CAMLOG[... cameracaptured]` line with `ClientStartedSession:1 ... bg:1` or `shouldRun:0` | the session server holds the app's session back: `bg:1`, the app is seen in the background (the display layout as cameracaptured sees it); `interruptions`/`cameraStolen`, another client has the camera |

The rear camera: the same test with `cam_s9=7 cam_rear=1 cam_trace=1`. With the run-time switch (`cam_switch=1`, scion-kernel
PR 341) the same lines are expected, and the flip button's tap gives `trace: vendor: copy streams positions (...)` with the
other position, then `stream: the Back Camera's frames stop for the Front Camera's` (or the other way round).
