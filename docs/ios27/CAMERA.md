<!-- SPDX-License-Identifier: MIT-0 -->
# The camera on iOS 27 (Galaxy S9)

State, 17 November 2026: **built, not run on a phone.** The kernel's camera driver has run on the S9 under iOS 27's kernel
(the rear camera's stream, 12 November; scion-starlte `CAMERA.md`), and the capture device below streamed the S9's front
camera into the stock Camera app on iOS 16.5 (3 November). The iOS 27 pieces on this page (the library in cameracaptured,
the root step, the device-tree node) have been built and checked on a copy of a root; no phone has booted them. The phone
test at the end is what is left.

What it gives when it works: the Camera app on the home screen, its preview from the S9's camera, and a photograph. One
camera per boot: the front one (`cam_s9=7`), or the rear one (`cam_s9=7 cam_rear=1`). The app's back and front cameras
both show the camera of that boot (below, "One camera per boot"). With `cam_switch=1` as well, both cameras in one boot,
the app's button switching between them (below, "Both cameras in one boot"; written and checked on the build machine,
**not run on a phone**).

## Turning it on

1. The root: step `94-camera` (`tools/ios27/fullroot/steps.d/94-camera.sh`, on by default, `ENABLE_CAMERA=0` leaves it out)
   builds `/usr/lib/cam.dylib` with your Xcode and records it for cameracaptured. Optional: `CAMERA_TUNING_DIR`, colour
   tunings `camera-<sensor>.isp` made from your own phone's files (`tools/camtuning-exynos.py`); without them the pictures
   have no colour matrix and no lens shading (right, and flat).
2. The boot image, made again after the root, with the kernel's camera asked for:

       tools/ios27/s9/make-boot-image.sh --kernel hakios-kernel --donor-adt inputs/n104ap.adt --template inputs/recovery.img \
           --cmdline 'sbgl=0 sweetspeed=442337279 cam_s9=7'              # the rear camera: ... cam_s9=7 cam_rear=1

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
| Formats | named as the iPhone X's (`D22/AVCaptureSession.plist`: back "Photo" 4032 x 3024, "Video-60" 4224 x 2376; front 3088 x 2320, 3840 x 2160) | iOS 27's CMCapture has only the iPhone 11's `N104/AVCaptureSession.plist`: back "Photo" 4224 x 3168 at 60, "Video-60" 4224 x 2384; front "Photo" 4032 x 3024, "Video-30" 4032 x 2272 (`HAKIOS_CAMERA_N104` in `camera_device.c`); matched by more than size: see "The second cause" below, `tools/ios27/camera/camera_formats.c` |
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

Without `cam_switch=1` the S9's driver drives one sensor per boot: the front S5K3H1 unless `cam_rear=1`, then the rear
S5K2L3. The node's frames carry the sensor number last asked for (`HAKIOS_CAMERA_SENSOR` only relabels them: `asked_sensor`
in `hakios.exynos_camera.c`), so the device's back stream (sensor 1) and front stream (sensor 2) both get the boot's camera.
This is the default, and `cam_switch` changes nothing of it when absent or 0.

## Both cameras in one boot (`cam_switch=1`)

**Not run on a phone.** With `cam_s9=7 cam_switch=1` the driver has both cameras in the run, one streaming at a time,
starting with the one `cam_rear` names (the front without it). The node's `HAKIOS_CAMERA_SENSOR` (1 the rear camera, 2 the
front), which `camera_device.c` already sends when its stream's frames are another camera's, changes the camera: the
running one is stopped and powered down exactly as a stream that nobody reads any more is, the other one's numbers are put
in (its CSIS, D-PHY, WDMA, lanes, mode, sensor bus, lens), and it is brought up exactly as the first read of a boot brings a
camera up. The S9+'s driver (`hakios.exynos_camera_star2.c`) switches the same way: one stream ends, the next begins with
the camera asked for. The protocol is unchanged: the frames' header says which camera took each frame and its size
(1632 x 1224 for the front, 2016 x 1512 for the rear), and `camera_device.c` makes its buffer again when the size changes,
as it does for the S9+'s cameras; nothing on the iOS side changes, and PR 328's `cam.dylib` needs nothing more.

What the driver guarantees (`drivers/camera/exynos/hakios.exynos_camera_switch.h`, checked on the build machine by
`tools/exynos-camera-switch-test.sh` under the sanitizers, in CI):
- never two cameras powered: the other camera is brought up only after the running one is down and no reader holds one of
  its frames;
- no frame of the old camera is given after the ask (the ring's newest frame is dropped at once), and a frame being copied
  out when the ask comes is refused (`EAGAIN`, counted as `dropped`): a reader gets the new camera's frames or an error,
  never one camera's frame labelled as the other's;
- a change whose new camera does not come up (no answer on its bus, the domain not coming on, no frame) ends with
  everything down, **both cameras off**, and readers get `EIO`; the next open tries the camera asked for again;
- two changes in quick succession are taken in turn; one asked back before the stream saw the first is no change.

The rear camera's stabiliser and iris are put in their safe state when it powers down, as at the end of any rear stream
(the iris back at F1.5, the servo off and the gyro asleep, before its supplies go). Nothing writes the lens's EEPROM; the
iris camera and its IR LED are never touched. With `cam_af=N` each camera's own lens is driven while that camera streams
(the two lenses' state is kept apart, `hakios.ak737x.c`). `cam_s9_exp`, `cam_s9_ispdiv` and `cam_s9_addr` apply to the
boot's first camera only; the other one starts from its own defaults (the rear camera's 466 MHz ISPPRE bus clock, without
which CSIS0 overflows, among them).

## Not done

- Run on a phone: everything on this page.
- Switching cameras within a boot: written (`cam_switch=1`, above), not run on a phone: the phone test below.
- The front camera's focus motor from iOS (16.5's `inject_media.c` gives the iPhone X plist's front camera the back camera's
  autofocus keys; N104's plist has the same layout, not done here).
- Photographs in HEIC need the software encoder registration to work in cameracaptured as it did in mediaserverd; if it
  does not, the app's Formats setting "Most Compatible" (JPEG) is the way round.

## The phone test

Boot arguments: the root with `94-camera` (check the build's line `94-camera: /usr/lib/cam.dylib built ...` and
`make-boot-image: ... adt-isp-node: /arm-io/isp with camera-front and camera-rear`), `--cmdline 'sbgl=0 sweetspeed=442337279
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

### The cause, found by the trace (17 November, 20:35)

A second boot of phone 1 had `cam_trace=1` and the library also in Camera.app. Its log names the stop. The app connects
to cameracaptured's capture source and capture session servers. Then tccd answers the camera's access request with
`kTCCErrorDomain Code=6 "server error: Failed to issue Sandbox Extension for kTCCServiceCamera"`. The app's
`AVCaptureDeviceInput` then fails (`AVFoundationErrorDomain Code=-11852 "Cannot use Front Camera"`, not authorized), its
mode-and-device command fails (`-32200`, cannot capture), and no session ever asks the vendor for the camera.

tccd needs a sandbox extension to hand the app with its answer. The fork's sandbox stand-in issues one only when the
boot line's `sweetspeed` has the bit `0x2000000` (`bsd/kern/sweet_amfi.c`, `SWEET_SPEED_SBX_EXT`); without it the issue
call answers ENOENT. Both camera boots of 17 November used `sweetspeed=408782847` (`0x185d87ff`), which lacks that bit.
**The camera needs `sweetspeed` with `0x2000000`**: `sweetspeed=442337279` is the value above with it. The microphone's
request fails the same way (Live Photos), and this root has no audio server anyway, so the app's audio input still fails.
Neither has been tried on a phone yet.

Also in that log: `Attempting to use an unsupported session preset AVCaptureSessionPresetPhoto on device ... Front Camera`.
It may only follow from the failed input, or the device's formats may not match N104's presets. The traced boot with the
corrected `sweetspeed` decides it.

### The second cause: the cameras had no formats (17 November, 20:45)

The third traced boot had `sweetspeed=442337279`. tccd then allowed the camera (`kTCCServiceCamera ... AuthRight: Allowed`)
and the app went on to configure its session, and stopped one step later, the same on the front and the back camera (and
on a later boot with the root's audio daemons running, so audio is not it):

    Attempting to use an unsupported session preset AVCaptureSessionPresetPhoto on device <... [Front Camera] ...>
    Failed to add input <AVCaptureDeviceInput ... [Front Camera]> to session <AVCaptureSession ... [AVCaptureSessionPresetHigh]>
    CAMModeAndDeviceCommand configuration failed! (... CAMModeAndDeviceCommandCannotCaptureErrorDomain Code=-32200 ...)

The first line is CameraUI's (`-[CAMModeAndDeviceCommand executeWithContext:]`): the device said it takes no Photo preset.
iOS 27's capture stack makes a camera's AVCaptureDevice formats from N104's `AVCaptureSession.plist`: each format there
names a stream format of the plist's `FigCaptureStreamFormats` ("12MP" names "Photo", "1080p30" names
"Video-FullSensor-FES-30"), and CMCapture looks for that stream format in the stream's `SupportedFormatsArray`
(`csu_resolveFormatIndexForNamedStreamFormat`, 24A446's shared cache: its logic only). It compares the sensor size, binning,
sensor HDR, the front-end scaler's output size, `SIFREnabled` and the maximum frame rate. N104's "Photo", "Video-60" (back)
and "Video-30" (front) have `SIFREnabled`; `camera_device.c`'s formats did not say it. So no stream format was found, every
AVCaptureDevice format was dropped, and the cameras had no formats and no presets.

`tools/ios27/camera/camera_formats.c` now gives the streams N104's formats on iOS 27: back "Photo" (4224 x 3168, 60),
"Video-60" (4224 x 2384), "Video-FullSensor-FES-30" (4224 x 3168, scaler 2304 x 1728, also 1920 x 1440's); front "Photo"
(4032 x 3024), "Video-30" (4032 x 2272), "Video-FullSensor-FES-30" (scaler 2112 x 1584) and "Photo"'s companion without
SIFR. The sizes are the plist's; the pictures are scaled from the S9's sensors as before, at 30 frames a second. SIFR is the
iPhone sensor's second, short exposure: the S9 has none and none is made. iOS 16.5's formats are unchanged. **Not run on a
phone yet.** With the trace on and the library in the app, Camera.app says each camera's formats as AVFoundation has them
and its answer for each preset: `trace: app: Front Camera has N format(s)`, `trace: app: ... takes AVCaptureSessionPresetPhoto:
yes`. `NONE` there means cameracaptured still found none of the plist's formats.

### The third cause: cameracaptured never sees the app in the foreground (17 November, 21:30)

With the formats (above), phone 1's next boot had the cameras with formats (the back camera 16), the Photo preset taken
and the session configured, and then interrupted at once: `AVCaptureSession was interrupted for
VideoDeviceNotAvailableInBackground`, while the app was on the screen (RunningBoard: `running-active-Visible`). The
capture session server lets a client use the camera only while it is in the foreground, and its
`FigCaptureClientApplicationStateMonitor` (CMCapture) joins RunningBoard's state with the main display's layout from
FrontBoard: the client must be among the layout's foreground applications. cameracaptured's own log (the trace's os_log
file) says `FBSDisplayLayoutService: timed out waiting for initial layout update from com.apple.frontboard.main-display;
publishing nil`, and no layout came after it; SpringBoard's own layout monitors logged `layout: (null)` too. FrontBoard's
display layouts are not published on this port. Why is not known yet; it is FrontBoard's business, not the camera's.

`tools/ios27/camera/camera_layout.c` (cameracaptured, with the kernel's camera only): when the main display's layout
monitor hands a client a nil layout, the client is given a layout of that display with its own application in front.
Every real layout goes through unchanged, and RunningBoard's state still decides, so an application that leaves the
screen is still in the background. Said once a client: `hakios: camera: layout: FrontBoard gave the main display no
layout: com.apple.camera is taken as its foreground application`. With `cam_trace`, `camera_statetrace.c` also says the
layouts that do arrive and the monitor's states (`hakios: camera: trace: state: ...`). **Not run on a phone yet.**

### What phone 1 showed after that (17 November, 22:42 to 23:31), and the next test

- FrontBoard does publish a main-display layout, but the layout has no elements, `backlightLevel = 0` and `backlightState = 0`,
  while the screen is on. CMCapture finds no foreground app in it. `camera_layout.c` takes a client as the foreground app
  when the main display's layout is nil, or when FrontBoard's own layout names no application at all. With it,
  cameracaptured said `state: Foregrounded (bksAppState: ForegroundRunning, layout: Fullscreen)`, registered Camera.app,
  copied the device and asked for its streams, and the kernel's frames flowed. Why SpringBoard publishes an empty layout
  is open, and it is not the camera's problem: SpringBoard turns the backlight on at boot (`Off->ActiveOn`), yet the layout
  it hands out never carries it.
- cameracaptured then aborted three times in turn while building the session's graph (the text comes from `sweetscan=…,cameracaptured`):
  1. `Unknown pixel format ()`: iOS 27's camera source node weighs the scalers against an `IntermediateTap` output, which
     our formats did not list. `camera_formats.c` lists one, BGRA only, so the node keeps the scalers.
  2. `Output can't be nil` in the video thumbnail sink (Camera.app's live filter previews). `camera_refuse.c` builds
     sessions without video thumbnails.
  3. `Output can't be nil` in the movie file sink's scaler (Photo mode's Live Photo movie). `camera_refuse.c` answers NO to
     the Iris sink's movie capture, as iOS 16.5 had Live Photo off.
- The formats behind a front-end scaler (1080p30, 720p30, 1920 x 1440) are not offered for now. The first abort happened with them,
  but it was abort 1 above, so they may come back once the preview works.

The next test (dylib `-9` of this branch's `build.sh`, the boot line with `sweetspeed=442337279 cam_s9=7 cam_trace=1
sweetscan=backboardd,SpringBoard,cameracaptured`): tap Camera, wait 30 s, screenshot; tap the flip button, wait 15 s, screenshot;
press the shutter, wait 15 s, screenshot; open Photos. Expected lines: `thumbnails: ... replaced`, `live photo: ... replaced (NO)`,
`layout: FrontBoard gave the main display no layout: com.apple.camera ...`, `state: Foregrounded`, no `CRASH cameracaptured`,
`trace: stream: set (Back Camera) VideoOutputConfigurations`, `stream: the graph starts the stream`, `PrimaryScaler: the first
picture given`. If it aborts again, look for the `STR *** Terminating app due to uncaught exception` line.

### The fourth cause: the stream's colour info was empty (18 November, 00:00)

Phone 1 booted dylib `-9` (17 November, 23:47). cameracaptured did not abort any more; Camera.app's session registered,
copied the device, asked for the back camera's stream and set its outputs, and then failed at once: `Received a session
runtime error notification: AVFoundationErrorDomain Code=-11800 ... (-12780)`. The app retried about thirty times a second
(register, copy device, streams asked for, given back), the viewfinder stayed black, the shutter did nothing, and the flip
button gave the same loop on the front camera. The last thing the stream was asked before each give-back was
`VideoOutputsColorInfo`, answered `{ }`.

iOS 27.0.1's CMCapture (24A446's shared cache, its logic only): the source node, bringing the stream up to date for a session
(`-[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:]`, `_bringStreamUpToDate`,
`_verifyColorSpacePropertiesForAllOutputs`), reads the stream's `VideoOutputsColorInfo` and, for every output whose colour
space properties are not 0, wants that output's entry to equal `+[BWVideoFormat pixelBufferAttachmentsForColorSpaceProperties:]`
of the node's `_colorSpaceProperties` (a dictionary of colour attachments); anything else returns -12780 and the session
fails. Apple's ISP plug-in answers the property itself with each output's colour attachments; the graph never sets it.

Two answers were tried on phone 1 the same night, both with the same loop. Dylib `-11`'s wrap of
`allocateOrReuseBufferPoolsFromSourceNode:` refused itself (the method returns an `OSStatus`, an int, which its check did not
allow). Dylib `-13` (00:15) answered every output with `pixelBufferAttachmentsForColorSpaceProperties:` of the node's
`_colorSpaceProperties`, which was **0** (and the attachments of 0 are nil), and the sessions still failed: the output that
fails has properties that are not the node's. In 24A446 the scalers' properties are the node's, and the still image output's
come from its own method (firmware still image output, 10-bit, ...); the per-output methods are direct methods, out of the
Objective-C runtime's reach, so which output and which properties could not be read from outside.

Which output, read from 24A446: the scalers' colour space properties are the node's (0 here: "N/A", not checked); any other
output's (the still image output's) are, when the still image output is the firmware's and wants a 10-bit pixel format, 1
("sRGB/601") or 3 ("P3/601", for a wide colour space), else the node's again (`BWColorSpacePropertiesToString`'s table: 0 N/A,
1 sRGB/601, 2 sRGB/709, 3 P3/601, 4 P3/709, ...). With the node's at 0, the only value that fails is 1: the still image output
wants sRGB/601.

What the S9's pictures are (`camera_isp.c`): sRGB's curve, BT.601's matrix (420f full range, 420v video range), the sensor's
colours white-balanced and taken as sRGB's primaries: sRGB/601, which is also the only colour space the streams offer
(`SupportedColorSpaces`: sRGB). So `tools/ios27/camera/camera_colour.c` (dylib `-15`, always on in cameracaptured with the
kernel's camera) answers the truth: every output's entry of `VideoOutputsColorInfo` is `pixelBufferAttachmentsForColorSpaceProperties:1`
(sRGB/601), and every picture `camera_device.c` makes carries those same attachments from the first session on
(`hakios_camera_frame_colour`; before, BT.601 matrix and BT.709 primaries and transfer). It also wraps the attachments method to
remember what it was asked for (unchanged) and `allocateOrReuseBufferPoolsFromSourceNode:` to say an update that still fails:
`colour: the source node's update FAILED (N): node colour space properties P; attachments asked K time(s) in the update, for
properties ...`, so that one boot shows both sides. A check that asks for 3 would mean the session wants wide colour (P3),
which the pictures are not: converting them is a known gap below. `cam_colour_any=1` on the boot line (a diagnostic, off by
default) answers instead with entries that read as whatever attachments the check asked for last on that thread (dylib `-14`
did only that): a session that then runs while `-15`'s does not would show that the values differ. **Not run on a phone yet.**
Every name it uses is in 24B156's CMCapture too.

The still image output wanting a 10-bit pixel format (`stillImageOutputRequires10BitPixelFormat`) also means the photograph may
come in a 10-bit pool ('x420' or the like), which `camera_device.c` does not draw into (`StillImage: ... buffers are not for this
ISP`): the next gap if the shutter gives nothing.

### Where the pictures stop: the flow trace

With `cam_trace=1`, `tools/ios27/camera/camera_flowtrace.c` follows the frames through cameracaptured's graph: which of the
stream's outputs the source node configures, enables and gives handlers to (`trace: graph: ...`), the first sample each node
output emits (`trace: flow: <node>'s output <name> emits its first sample: WxH 'fmt' IOSurface, time stamp N ms from now`),
every 10 seconds how many each has emitted (`trace: flow: emitted so far: ...`), each node's dropped samples with their reason
(`... DROPS a sample`), and the preview stitcher's and the preview image queue sink's first samples, with the sink's image queue
(`nil` there: nothing reaches the screen). A black viewfinder is then one of: no `the graph starts the stream` (the session
stopped before, see the CAMLOG lines); the source node emits but the stitcher or the sink never renders (the node between them
drops: its reason); the sink renders but the app shows nothing (the image queue, the IOSurfaces' format, or the compositor:
`native-metal` refusals in backboardd's lines); a time stamp far from 0 ms from now (the queue holds frames for later).

### Why the display layout is empty: the publisher's trace

`camera_layout.c` stands in for FrontBoard's main-display layout in cameracaptured, which on this port names no application and
says `backlightState = 0`, `backlightLevel = 0` while the screen is on (above). SpringBoard publishes that layout through
FrontBoardServices' `FBSDisplayLayoutPublisher`: elements added and removed for what is on the screen (`_addElement:forKey:`,
where `addElement:`, `add:` and `remove:` lead), the backlight's state and level, publishing held by suppression and transition
assertions with a reason, and `flush`. With the library also inserted into SpringBoard (the root step with `CAMERA_TRACE_SB=1`, or
`,SpringBoard` added to the boot line's `sweetinject=` entry of `cam.dylib`) and `cam_trace=1`, `tools/ios27/camera/camera_sbtrace.c`
says each of those calls (the first 40 of a kind, then every 200th): `trace: layout publisher 0x...: add element (N): ...`,
`backlight state S`, `backlight level L`, `layout SUPPRESSED for "reason"`, `a suppression ends`, `transition "reason"`,
`flushed (N): <the layout>`. One boot then names the cause: no `add element` at all (SpringBoard's scene layout never reaches the
publisher), a `SUPPRESSED` with no `suppression ends` (published layouts held empty, the reason says by what), or no `backlight
state` (the backlight's state, from BacklightServices, never reaches it). Without the trace, or without the record naming
SpringBoard, the library does nothing in SpringBoard. **Not run on a phone.**

### What the S9's sessions are built without, and what restoring each needs

| Left out | Where | Why | To restore |
|---|---|---|---|
| Video thumbnails (the filter and style pickers' live previews) | `camera_refuse.c` | the preview pipeline makes no `videoThumbnailSinkOutput` for the S9's stream; the thumbnail pipeline was built on nil and cameracaptured aborted | find which node output N104's preview pipeline feeds them from (a thumbnail output of the stream, or a scaler node) and offer it |
| The Live Photo movie | `camera_refuse.c` | the movie file sink's scaler was built on a nil video capture output: aborted | a video capture output for Photo mode (the source pipeline's), and the sound around the shutter (no audio server on this root) |
| Formats behind a front-end scaler (1080p30, 720p30, 1920 x 1440) | `camera_formats.c` | cameracaptured threw sizing an output whose pixel format it did not know (with `IntermediateTap` missing, abort 1 above) | offer `Video-FullSensor-FES-30` again now that `IntermediateTap` is listed, and check the outputs' pixel formats |
| `IntermediateTap` frames | `camera_formats.c`, `camera_device.c` | listed (BGRA) so the graph can weigh it, but no frame is made for it | deliver it when the graph enables it (the flow trace says whether it does) |
| The outputs' real colour info | `camera_colour.c`, `camera_device.c` | the stream's `VideoOutputsColorInfo` answers whatever the graph's check asks for, and the frames carry BT.709 attachments whatever the output's colour space | answer each output's own colour space (as Apple's ISP plug-in does) and convert the pictures' colours to it |
| SIFR (the iPhone sensor's second exposure) | `camera_formats.c` | the S9's sensors have none; the formats say SIFR only so that N104's names match | nothing to restore |

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
2. Boot line: `sweetspeed` with the sandbox extensions' bit, and `cam_trace=1`: `--cmdline 'sbgl=0 sweetspeed=442337279
   cam_s9=7 cam_trace=1'` (408782847, the line of 17 November, lacks `0x2000000`: see "The cause" above).
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

## The phone test of the switch (`cam_switch=1`)

Prove the one-camera boots first (the table above, front and then rear). Then, first without iOS's camera, the kernel's own
reader (`cam_s9_selfread`, which opens and reads the node through the file system as a program does):

    --cmdline 'sbgl=0 sweetspeed=408782847 cam_s9=7 cam_switch=1 cam_s9_cached=1 cam_s9_selfread=900 cam_s9_selfread_switch=150'

thirty seconds after the boot it reads 900 frames and asks for the other camera after every 150 (front, rear, front ...).
From the USB console, `sysctl kern.hakios.camera_switch.sensor=1` (or `=2`) asks for the rear (front) camera of a stream
being read; `sysctl kern.hakios.camera_switch` gives the counters (`asked`, `done`, `failed`, `superseded`, `refused`,
`dropped`, `step`, `ms`). Then with the Camera app: the same boot without `cam_s9_selfread*`, the app opened, its button
pressed a few times each way.

Expected lines in the console log, in order, for a good change from the front to the rear camera:

| Step | Look for | What it means if it is the last line |
|---|---|---|
| start | `exynos_camera: cam_switch=1: both cameras in this run, the front camera first`; step 1's lines for BOTH cameras (`1: S2MPB02 ID`, the four front LDOs, `rear: 1: S2MPB03 ID ...`) | `switch: the rear camera's supplies could not be read: the front camera only in this run`: the S2MPB03 or its bus; SENSOR 1 is refused (`... is not offered in this run`) |
| 1 asked | `switch: 1: the reader asks for the rear camera (sensor 1): no more frames of the stream that runs are given ...` (selfread: `selfread: HAKIOS_CAMERA_SENSOR written ... taken`) | no line: the write did not reach the node, or `cam_switch=1` is not on the line |
| 2 stopped | `switch: 2: the front camera's stream stops ...`, `5: the sensor's frame count 0xff after the stop`, `7: the stream ends (the other camera is asked for)` | the thread is stuck in a frame wait: `no frame end in 300 ms` lines |
| 3 down | `switch: 3: the front camera powered down ...`, `powered down: MCLK and reset low, the bus pins inputs, BUCK2 and the LDOs off as found`, `the ISPPRE domain off again ...` | `NOT ALL OFF (a write failed)`: a supply's write did not read back; nothing more is written in the run |
| 4 readers out | `switch: 4: the front camera is off and no reader holds a frame of it (powered down, the ISPPRE domain off, every write read back)` | `NOT POWERED DOWN` or `the ISPPRE domain left on`: stop and look at the lines above |
| 5 the side | `switch: 5: the rear camera's way in: CSIS0, 4 lanes, 2016 x 1512`; the first time, `switch: 5: CSIS0, its D-PHY and its WDMA mapped` | |
| 6 up | `switch: 6: ...`, `rear: 2: THE SENSOR ANSWERS at 0x10: model ID 0x20c3`, `3: ISPPRE on`, `the D-PHY of CSIS0` | `rear: 2: the sensor does not answer` then `switch: FAILED at step 6 (the rear camera): ... BOTH cameras are off; readers get EIO until the next open` |
| 7 streaming | `switch: 7: ...`, `5: STREAM ON: 0x0100 = 0x0103`, `the sensor's frame count` not 0xff | `switch: FAILED at step 7`: the mode or the link (the rear camera's own run, scion-starlte `CAMERA.md`) |
| 8 done | `switch: 8: THE REAR CAMERA'S FIRST FRAME (2016 x 1512, frame N) M ms after the ask: the change is done (1 done, 0 failed)`; selfread: `the first frame of sensor 1 (2016 x 1512 ...) M ms after the ask` | `switch: FAILED at step 8`: no frame came (`no frame end in 300 ms` five times) |
| the reader | selfread's summary: `the switch: K change(s) asked, K gave the other camera's frames, 0 frame(s) WRONG` | any `selfread: WRONG: frame ... is sensor S's ...`: a frame of the other camera after the ask; the switch is broken, report the log |

From the rear camera back to the front: the same steps with the cameras' names swapped (step 3: `rear: OIS: the servo
switched off` if `cam_rear_ois=2`, the iris back at F1.5 if `cam_rear_ap` moved it, `rear: powered down: ... off as found`;
step 6: `2: THE SENSOR ANSWERS: model ID ...` (the S5K3H1's); step 8: `THE FRONT CAMERA'S FIRST FRAME (1632 x 1224 ...)`). In the
Camera app: the preview turns to the other camera within a second or so of the button (cameracaptured's log:
`hakios: camera: the frames changed (sensor 2, 1632x1224, now 1, 2016x1512): the buffer made again`), and the photograph
is of the camera shown.

A change that fails leaves both cameras off and the node giving `EIO` (the Camera app's preview stops: `the kernel's
camera has had a fault`); reopening the app opens the node again and tries the camera asked for. A boot without
`cam_switch=1` gives the phone back as it was.
