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

### The fifth cause: no pixel format for the still image output (18 November)

N104's `AVCaptureSession.plist` says `UsesPacked10BitFirmwareStillImageOutputPixelFormat` for the photo formats of both
cameras ("12MP", the front camera's "7MP") and for 1080p, 720p and 1920 x 1440: the A13's ISP gives its photographs in a
packed 10-bit format. iOS 27.0.1's CMCapture (24A446's shared cache, its logic only) then sets the source node's
`stillImageOutputRequires10BitPixelFormat`, and `-[BWMultiStreamCameraSourceNode _supportedPixelFormatsForOutputID:]`
keeps, of each output's `SupportedPixelFormats` in the stream's format, the ones whose range, bit depth, 4:2:2-ness and raw-ness
match what the output wants: for the still image output full range, 10-bit exactly when that flag is set, not 4:2:2, not raw,
and no more lossy compression than the session allows. The other outputs follow the session's client video format (420f
for Photo mode). The S9's streams offered 420f, 420v and BGRA, so with the flag the still image output was left with no
pixel format at all, and the session that builds its pools for every output could not start.

Dylib `-17`: `camera_refuse.c` answers NO to `-[FigCaptureSourceVideoFormat usesPacked10BitFirmwareStillImageOutputPixelFormat]`
(`still: ... replaced (NO: the still image output takes 420f)`, and at a session `still: the format wants a 10-bit still
image ... answered NO`): the still image output takes 420f, which `camera_device.c` draws. `cam_still10=1` on the boot line
keeps the plist's answer; for that case `camera_formats.c` also offers `xf20` (10-bit full range, two planes, each sample 16
bits with its 10 bits at the top) on the StillImage output, and `camera_tenbit.c` draws the picture in 8 bits into a
scratch 420f buffer and widens it into the pool (`StillImage: the graph's pool is 10-bit (xf20): ...`). The preview does
not need 10 bits: its outputs take 420f, a two-plane IOSurface that the Metal bridge samples (`newTextureWithDescriptor:iosurface:plane:`
with its YCbCr planes, `tools/metal/bridge/NativeMetalObjects.rs`). **Not run on a phone yet.**

### The sixth cause: outputs the graph configures that the stream did not keep (18 November)

Phone 1 ran dylibs `-17` to `-20` on 18 November (front and back camera, `cam_trace=1`). With `-17` the still image output had
its 420f, and cameracaptured still aborted at every session: `Internal inconsistency, output pixel format () doesn't match the
pixelBufferPool's pixel format (420f)`. The throw is in `-[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:]`
(24A446, its logic only): after setting `VideoOutputConfigurations`, the source node reads the property back from the stream
and wants each output's `PixelFormat` to equal its pool's. Two things made it differ:

1. The graph configures `IntermediateTap` and `Depth`, which `camera_device.c` did not keep (`stream: an output not here`), so
   they were missing from the read-back. On iOS 27 `IntermediateTap` is now a fourth output, drawn as the scalers are, and the
   configurations of outputs that are not drawn are given back as they were set (dylib `-18`).
2. The graph's own configuration of `PrimaryScaler` (1504 x 1128 in Photo mode) carries `PixelFormat` 0. The source node takes a
   streaming output's pixel format from its node output's video format, which nothing downstream resolves on the S9 (the Live
   Photo movie that would use it is left out, above), while the output's pool, shared with the other streaming outputs, is
   420f. `camera_outputs.c` reads a configuration whose `PixelFormat` is missing or 0 back as 420f (dylib `-20`;
   `-19` filled only a missing one). Each set of configurations, enables and handlers is said on one line: `outputs: Back
   Camera: VideoOutputConfigurations: StillImage (format 420f, 4032x3024, ...), PrimaryScaler (...)`.

With `-20` (11:00) the graph ran for the first time on iOS 27: no abort, `the graph starts the stream`, `PrimaryScaler: the first
picture given (1504x1128 420f)`, the source node's `Preview(PrimaryScaler)` output emitting about eight pictures a second
(`camera_isp.c` on the CPU), the shutter giving a 4032 x 3024 picture to the still image coordinator, and the flip button
changing to the front camera. Still missing: the screen stayed black (no node after the source node emitted anything; the
viewfinder was open, so the frames stop inside cameracaptured's graph), and the photograph failed (`-11800`, underlying
`-16808`, 10 ms after the shutter, before the picture was made) and was not saved. Dylib `-21` follows each output to the node
it leads to (`camera_flownext.c`: `trace: flow: ... leads to <class> "<name>"`, `... renders a sample ...`).

### Where the preview stops now (18 November, dylibs `-21` to `-26`, 24A446 and 24B156)

- The source node has 21 outputs. Only `StillImage` (to the still image coordinator) and `DetectedObjects` (to face
  detection) are connected; `Preview(PrimaryScaler)` leads nowhere (`camera_flownext.c`).
- The session's parsed configuration has one preview sink with a video preview connection, and the camera source has its
  video preview sink connection (`camera_previewtrace.c`). `FigCapturePreviewSinkPipeline` is made, from an output of the
  "Back Camera" source node with an empty name, with no image queue sink given, and its build returns success.
- The graph's connections (`camera_connecttrace.c`): the still pipeline asks for and gets all of its own (coordinator, frame
  coordinator, format converter, `BWUBNode` "UB", scaler, Meteor headroom, encoder, still image sink), and the metadata path
  its own. The preview pipeline asks for **none** through `-[BWGraph connectOutput:toInput:pipelineStage:deferredAttach:]`.
  Why: the seventh cause, below (no Metal device in cameracaptured). Before it was found, the next step was to find out what `_buildVideoPreviewSinkPipeline:...` (a direct call from the init) does on the S9, for
  example a stitcher or image queue sink that it does not make and that it skips without failing.
- The photograph goes through `BWUBNode` (unified bracketing), whose processing setup returns -16808 on the S9 even with the
  formats' fusion answered NO (`camera_stills.c`). The still pipeline has to be built without UB.
- 24B156: inserting `cam.dylib` into SpringBoard (the layout publisher trace) stalls SpringBoard at the Apple logo. Insert it
  into cameracaptured and Camera only.

### The seventh cause: cameracaptured has no Metal device (18 November, 24B156)

Read in iOS 27.2.1's CMCapture (24B156's shared cache, its logic only): the first thing
`-[FigCapturePreviewSinkPipeline _buildVideoPreviewSinkPipeline:...]` needs, before it makes any node, is a Metal device
(`+[FigMetalContext metalDevice]`, CMImaging's, made once per process), a command queue on it (`newCommandQueue`) and the
pipeline's two Metal dispatch queues (`_metalCompletionQueue`, `_metalSubmissionQueue`). When any of the four is nil, the
build makes a debug assertion (`FigDebugAssert3`, silent on a release build) and **returns 0, success**, with no node added
and no connection asked for. The same path (assertion, then success) follows a failed `addNode:error:` or
`connectOutput:...` of several of its nodes. The init takes the 0 as success and the pipeline is "made". That is exactly
what phone 1 showed with dylibs `-21` to `-27`: the build a success, the graph's connections all the still pipeline's and
the metadata path's, none of the preview's, `Preview(PrimaryScaler)` leading nowhere, a black viewfinder.

cameracaptured has no Metal device on this root. The Metal plug-in (`StarbiteMetal.bundle`, `tools/ios27/metal2/HotMetal2.mm`
`listed()`) gives one only to the processes named in `/private/var/local/mtl2-clients`, which `80-metal-ca.sh` writes as
`backboardd` alone. Phone 1's consoles (v1-cam26, d23-cam25, d21-cam24) have `MTL2[pid cameracaptured]: plugin loaded` at
cameracaptured's start (Metal was asked for a device) and nothing after it (the plug-in's `not listed` line goes to
`mtl2.log` only). iOS 16.5's mediaserverd built its preview with no Metal device at all (`userland/src/inject_media.c` even
takes the device out of that process); iOS 27's build does not.

`tools/ios27/camera/camera_metal.c` (dylib `-28`):
- With `cam_trace=1`: `trace: metal: +[FigMetalContext metalDevice] -> nil` (or the device's name), once and when it
  changes, and the first 80 nodes the capture pipelines add (`trace: metal: <pipeline> adds <node class> "<name>": yes|NO`,
  `-[FigCapturePipeline addNode:error:]`). Without a device no `FigCapturePreviewSinkPipeline adds ...` line is expected.
- With `cam_metal=1` on the boot line (off by default): when the Metal plug-in is loaded into cameracaptured, its import of
  `fopen()` is pointed at a wrapper that, for the client list only, gives the file's text with `cameracaptured` added (a
  `-cameracaptured` in the file still wins). Nothing on the phone is written, no other process is touched. Said:
  `metal: cam_metal=1: the Metal plug-in is loaded: its fopen() of the client list taken`. The same without the switch, at
  root build time: `METAL_CA_CLIENTS="backboardd cameracaptured"` for `80-metal-ca.sh`. A Metal client wires about 57 MiB
  of GPU memory when its device is made (LIQUID-GLASS.md), and its first preview compiles CMCapture's shaders through
  metal2vulkan: whether all of them translate is what the phone test shows.

Checked on the build Mac: the library builds; the `fopen()` redirection, built for macOS around a stand-in plug-in, makes the
stand-in read the list with `cameracaptured` in it.

What phone 1 showed with dylib `-28` (18 November, 24B156, rear camera, `cam_trace=1`):
- Boot A (`ca-cam28`), without `cam_metal=1`: `trace: metal: +[FigMetalContext metalDevice] -> nil`; the camera source pipeline
  adds its source node and the still image pipeline its nodes, the preview pipeline none. Black viewfinder. The cause is
  confirmed.
- Boot B (`cb-cam28-metal`), with `cam_metal=1`: the plug-in's list taken, `MTL2[pid cameracaptured]: device Mali-G72 MC18`
  (about 2.8 s), the metal trace's device line (the device's `name` is not a string there, so it printed `-`). At the tap the
  preview pipeline is built for the first time: `Preview Governor` (BWFrameRateGovernorNode), `Preview Pre-Filter Splitter`,
  BWPreviewStitcherNode, `Preview Scaler` (BWPixelTransferNode), `Preview Splitter`, BWStreamingFilterNode, `Assistive Overlay`
  (BWAssistiveOverlayNode), each added and connected. Right after the last connection cameracaptured aborts (signal 6) on an
  uncaught Objective-C exception thrown from `___forwarding___` (an unrecognised selector), restarted by launchd, and aborts
  the same way at each of the app's retries. The viewfinder stays black. Also seen: two of CMCapture's compute pipelines
  refused by metal2vulkan at cameracaptured's start (`adjustEvmPyramidColor`: imageblocks, not implemented), and
  cameracaptured at about 150 MiB resident with 6 to 9 MiB of memory free.
- Dylib `-29` says, with the trace, each unrecognised selector (`trace: exception: -[Class selector] NOT RECOGNISED: callers ...`,
  from `-/+[NSObject doesNotRecognizeSelector:]`) and an uncaught exception's name and reason (`trace: exception: uncaught ...`).
- Boot C (`cc-cam29-metal`, dylib `-29`, `cam_metal=1`): `trace: exception: -[__NSCFNumber objectForKey:] NOT RECOGNISED`,
  uncaught `NSInvalidArgumentException`. The callers, read in 24B156's cache: VideoToolbox's
  `FigGetAlignmentForIOSurfaceOutputToDisplayService`, from `-[BWImageQueueSinkNode _updateInputRequirements]`, from the image
  queue sink's init in `_appendImageQueueSinkNodeToPreviewPipelineStage:...`: the last node of the preview. That function
  finds the display service (`AppleCLCD`, `AppleCLCD2` or `AppleRGBOUT`) and reads its `DisplayPipePlaneBaseAlignment` as a
  **dictionary** holding `PlaneBaseAlignmentLinear` (and `DisplayPipeStrideRequirements` holding `StrideLinearHorizontal`),
  64 when either is missing. The fork's `iokit/Kernel/arm/SweetFramebuffer.cpp` publishes `DisplayPipePlaneBaseAlignment`
  as the number 64. `CFDictionaryGetValue()` on a number sends it `-objectForKey:`, and cameracaptured aborts.
- Dylib `-30`, with `cam_metal=1`, in cameracaptured only: NSNumber gets an `-objectForKey:` that answers nil (said once,
  with its callers): VideoToolbox then keeps its default, 64, the value the kernel means. Checked on the build Mac
  (`CFDictionaryGetValue()` on a number gives NULL with it). The fix where the cause is would be the kernel publishing the
  property as Apple's display does (`{PlaneBaseAlignmentLinear = 64}`); not done here, because every process reads that
  property (QuartzCore among them) and it is not the camera's to change. **`-30` not run on a phone** when this was written.

### Dylib `-30` on the phone, and where the picture is lost (20 November, desk work)

Phone 1 booted dylib `-30` on 18 November (15:39, rear camera, the line `c272m`: `cam_trace=1 cam_metal=1`; console
`scion-phone1/evidence/cd-cam30-metal`, on the build Mac). Read from that console on 20 November, with no phone:

- **Verified in the console.** cameracaptured did not abort. The preview pipeline was built and connected (`Preview Governor`
  to `BWImageQueueSinkNode`), and every node on the path emitted: the source node's `Preview(PrimaryScaler)` 1504 x 1128
  `420f`, the stitcher, `Preview Scaler`, `Preview Splitter`, `BWStreamingFilterNode` and `Assistive Overlay` 828 x 1702 `420f`,
  each an IOSurface, about 8 a second, none dropped. The image queue sink rendered them (300 by 15:42:06) into its image queue
  (`<CAImageQueue ...>`, fences on). At the sink's first sample backboardd made a texture of format 500 (`YCBCR8_420_2P`,
  usage ShaderRead) of 828 x 1702 over the surface: `MTL2[57 backboardd]: native-metal: texture memory fmt 500 ...`. Neither
  process logged a Metal refusal while the camera ran (backboardd had used 18 of its 64 lines, so a refusal would have
  shown), and no `MISSING ... newSharedEventHandle` (a cross-process shared event, which the bridge does not have). Camera.app
  logged `DidDisplayFirstPreviewFrame`. The screenshot of the viewfinder is pure black, (0, 0, 0) at every point.
- **Inferred, not seen.** No second display layer: the framebuffer driver (`SweetFramebuffer.cpp`, the xnu fork) takes up to
  four layers in a swap but draws only 4-byte (BGRA) surfaces, so a `420f` surface on a layer of its own would be dropped
  without a line. The console has no `SWEET FB: layer N surface ... at` line (the driver says each position of a layer above
  the first) and no new framebuffer call after the camera began, so the window server most likely composited the preview
  with the GPU (hence the format 500 texture), not on a layer of the display. That swap log was used up before the camera
  ran, so this is not verified.
- **Inferred: the secure indicator does not blacken it.** iOS 27's CMCapture blackens or counts frames that its ISP marks
  (`BWInvalidFramesChecker`, the frame metadata's `InvalidDueToNoSensorAccess` and `SensorAccess`). `camera_device.c`'s
  frames carry neither, so the check sees valid frames. The console has no `blackened` line (CAMLOG takes these only when
  they name the camera).
- **Not checked by any trace: what the pictures hold.** The flow trace counts samples and says their size and format. Nothing
  read a pixel. A zeroed `420f` picture would show green, not black (Y 0 with Cb and Cr 0 converts to green). A black one
  is Y 0 or 16 with Cb and Cr 128. A YCbCr texture whose conversion never ran reads (0, 0, 0) with alpha 1. So pure black fits
  three places equally well, and the console cannot tell them apart:
  1. the pictures handed to the image queue are black (a node in cameracaptured, or its GPU writes not reaching the surfaces);
  2. backboardd draws the image queue but its format 500 texture reads black (the conversion in metal2vulkan on PanVK, or the
     surface's bytes as backboardd's GPU reads them);
  3. backboardd never draws the image queue's surfaces (the layer not drawn, or not by a texture over them).

**The trace that decides it** (this change; trace only, nothing else changes):
- `camera_pixeltrace.c` (cam.dylib, cameracaptured, with `cam_trace=1`): for the 1st, 30th and 300th sample of every node
  output, and of the image queue sink, the processor reads the picture through CoreVideo's read lock: a 16 x 16 grid of luma
  and an 8 x 8 grid of chroma. It reads them once as the cache has them, and once after cleaning and invalidating the
  picture's lines. Said: `trace: pixels: <node>'s output <name>, sample N: 828x1702 '420f' surface S: Y mean M least L most H,
  K of 256 above black; Cb B Cr R` (and `; after the cache's clean: ...` where the two differ).
- `tools/ios27/metal2/YCbCrTrace.h`, in `NativeBridgeMetalFrontend.mm` (StarbiteMetal, every Metal client, always on, at most
  24 lines a process): the same grid, read in that process, when a two-plane YCbCr texture over an IOSurface is made and
  when an encoder first binds a texture over each surface: `native-metal: ycbcr texture made: fmt 500 over surface S '420f'
  828x1702 plane 0: Y mean ...` and
  `native-metal: ycbcr texture bound (NBRenderEncoder, stage 2, slot N): ...`.

How to read one boot with both: the first `trace: pixels:` line on the path whose grid is black names the node in
cameracaptured (1). If the sink's pictures hold the room, backboardd's `ycbcr texture made` line for the same surface ID says
whether its pages hold it too. No `ycbcr texture bound` line means the surface is never sampled (3). Bound, holding the
picture, and still black on the screen means the conversion or the draw (2). Then the runtime's telemetry
(`/var/local/mtl2-telemetry` present, the rows in `/var/local/mtl2.log`) is the next thing to read. `TEST-ON-PHONE.md` of
the desk work (`~/s9/agents/camera-desk-1120` on the build Mac) has the steps. **Built, not run on a phone.**

### The preview stitcher writes nothing (22 November test, 23 November desk work)

Phone 2 booted PR 400's pixel trace on 22 November (line `c272m`, root `root-cam`; console
`scion-phoneB/logs/con-xnu-boot-c272m-1122-2011.log`, `mtl2.log` and cameracaptured's os_log from the root, on the build Mac).
The source node's `Preview(PrimaryScaler)` pictures are lit (rear: Y mean 13, dim; front: Y mean 99). The stitcher's output
surface reads `Y 0, Cb 48 Cr 48` at its 1st, 30th and 300th sample, the same as before anything ran: nothing in the stitcher
writes it. Every node after it reads Y 0, Cb Cr 128, and so does backboardd.

- **Verified in the files.** The compute binding `ycbcr texture bound (NBComputeEncoder, stage 0, slot 0): fmt 540 over
  surface 293` is not the stitcher writing: it comes after the stitcher's first output and after `Preview Scaler` takes it,
  and it is a read (usage 0x1). It is the Preview Scaler (VideoToolbox's Metal transfer session) reading the stitcher's
  output, then writing its own (R8Uint and R16Uint textures, usage 0x2). In cameracaptured no YCbCr texture over the camera's
  own surfaces was ever made (6 of 12 `ycbcr texture made` lines used, none for them), and no Metal refusal or missing
  selector was logged while the camera ran.
- **Verified in the disassembly (24B156 CMCapture and VideoToolbox).** The single-camera stitcher does no Metal of its own.
  `-[BWPreviewStitcherRenderer _transferPixelBuffer:...]` calls `-[BWPixelBufferTransferRenderer
  doPixelRotationTransferWithSession:...]`, which for an uncompressed `420f` picture calls `VTPixelRotationSessionRotateSubImage`
  (rotation and scaling in one step, the 1504 x 1128 picture into the 828 x 1702 one). A nonzero status goes only to a debug
  assertion: the stitcher emits its output either way. VideoToolbox builds the transfer up to three times, each time allowing
  one way less: the M2M scaler (IOSurfaceAccelerator, `AppleM2ScalerCSCDriver`, which the S9 does not have), its Metal
  transfer session, software. The Metal session is allowed only when `VTPixelTransferSessionCanUseMetalInTheBackground` says yes, that is when
  `proc_can_use_foreground_hw(getpid())` returns 1, or 0 with reason 13 (a daemon with no adopted voucher) or 16 (it adopted the
  voucher of an app in the background). That is a kernel call (`PROC_INFO_CALL_CANUSEFGHW`).
- **Inferred.** The Metal chain did not run for the stitcher: its source would have been read through a format 540 texture,
  as `Preview Scaler`'s is, and none was made. A software chain that ran would have written the picture. So either every
  attempt failed (a status that is thrown away), or a chain ran and wrote nothing.
- **Unrelated, inferred.** `Assertion failed _renderPipeline` and `PTEffectRendererSDOF is nil` (three times at 65 s) come from
  the portrait effect's set-up, before Camera opened. The stitcher's output is black from its first sample, 70 s later.
  cameracaptured also logged `compute pipeline (adjustEvmPyramidColor) refused: ... Imageblocks` then.

**The trace that decides it** (`camera_stitchtrace.c`, cam.dylib, cameracaptured, with `cam_trace=1`; trace only): at the
stitcher transfer's 1st, 30th and 300th call, and at the first call that returns each new status, it says
`trace: stitch: transfer N: status S, session there|NONE; from surface ... rect ... to surface ... rect ..., black fill ...` and
`trace: stitch: transfer N: proc_can_use_foreground_hw R reason K; VideoToolbox: Metal in the background yes|no, avoid
hardware ...` (asked on the calling thread, whose voucher the kernel reads), with the destination's grid before and after
(`trace: pixels: the stitcher's transfer N, destination before|after: ...`). Once: `IOSurfaceAcceleratorCreate: ...`. And
`trace: stitch: render N into surface ...: M transfers so far` for the stitcher renderer's own calls. How to read it:
a nonzero status with `Metal in the background no` names the kernel's answer (the reason says which branch); a nonzero
status with `yes` means the Metal chain itself fails (then the Metal transfer session); status 0 with the grid unchanged
means a chain that writes nothing; no `transfer` lines with `render` lines means the stitcher returns before its transfer.
Run on phone 2 on 23 November: the next section.

### What the stitcher trace showed: the M2M scaler's up-scale limit (23 November)

Phone 2 ran the trace on 23 November, as `TEST-ON-PHONE.md` of the desk work says (line `c272m`, `root-cam` with only
`cam.dylib` changed). Consoles: `scion-phoneB/logs/con-xnu-boot-c272m-1123-*.log` on the build Mac.

- **Measured.** At transfers 1, 30 and 300 the line was the same: `status 0, session there; from surface ... 1504x1128
  '420f' rect 753,565 0x0 to surface ... 828x1702 '420f' rect 0,178 828x1106`. With it came `proc_can_use_foreground_hw 0
  reason 13; VideoToolbox: Metal in the background yes` and `IOSurfaceAcceleratorCreate: -536870208, accelerator none`.
  The stitcher asks VideoToolbox to draw a **0x0 source rectangle** at the picture's centre. VideoToolbox succeeds with
  nothing to draw, so the destination is unchanged. The kernel's answer is not the cause: 0 with reason 13 is a daemon
  with no adopted voucher, and VideoToolbox then allows its Metal chain.
- **Read in 24B156's CMCapture and CMImaging.** In `-[BWPreviewStitcherRenderer renderStitchedPixelBuffer:...]` (render
  mode 0, the single-camera path) the up-scale from the source rectangle to the output rectangle is capped at
  `+[FigM2MController maxUpscale]`. When output/input exceeds the cap, the source rectangle is shrunk about its centre to
  output/cap. `maxUpscale` gives the M2M scaler's capabilities. `+[FigM2MController queryCapabilities]` reads them once
  from the IOService `AppleM2ScalerCSCDriver` (property `IOSurfaceAcceleratorCapabilitiesDict`: `...MaxHUpscale`,
  `...MaxVUpscale`, the down-scale limits, minimum sizes). The S9 has no such service, so the cap is 0x0 and the rectangle
  collapses to nothing.
- **Ruled out on the phone on the way** (a local diagnostic build, not in this repository): the renderer's camera infos
  are nil, and that is by design, because the stitching-disabled node makes none. Answering the frame pair's base zoom
  with 1 changed nothing. The input samples' metadata has no `ValidBufferRect`, so the bound is the whole buffer.
  `_getInputRect:...` and `_transferPixelBuffer:...` are not called on this path.
- **Confirmed by an experiment, not a fix:** the diagnostic build answered `maxUpscale` with 16x16 instead of 0x0. The
  source rectangle became 376,282 752x564, the rear camera's 2x crop. The stitcher's output went from Y 0 (0 of 256
  above black) to Y 9 (17 of 256). The camera lay face down, at Y 14 at the source. The picture reached the image queue
  sink (Y 9 at samples 30 and 300). The screen could not show it that night: the room was dark, and the front camera read
  `gain 64.0x, green 0 above black` at the source.

The fix this needs is a real M2M scaler on the port: an IOSurfaceAccelerator service that does the transforms
VideoToolbox and CMCapture send it, and publishes its true capabilities. The Exynos 9810's own memory-to-memory scaler is
one candidate, a GPU-backed one another. Answering the capability query with numbers nothing stands behind would be
faking hardware, and is not done. Once that service is there, the same phone test, in a lit room, should show the
source rectangle as above (not 0x0), the stitcher's pixel line lit like the source's, and `IOSurfaceAcceleratorCreate:
... accelerator made`.

### The M2M scaler on the port, and the next break (24 November)

The S9 now has the service the stitcher's limit comes from: `AppleM2ScalerCSCDriver`, a CPU scaler and colour-space converter
for 4:2:0 two-plane pictures with a truthful capability dictionary (M2M-SCALER.md: the interface iOS expects, the design,
the tests). On phone 1 (all **measured**, M2M-SCALER.md "The phone test"):

- `trace: stitch: IOSurfaceAcceleratorCreate: 0 (0), accelerator made`, and the stitcher's source rectangle is the picture
  again: 376,282 752x564 (the 2x crop), then 0,0 1504x1128; never 0x0.
- VideoToolbox's hardware attempt is this service: cameracaptured's transfers 1504x1128 to 828x1106 turned by 90 degrees
  (transform 4; the front camera's with the mirror, transform 6), about 15 ms each.
- The picture flows: front camera source Y 112, stitcher output Y 70 (160 of 256 grid points above black: the picture fills
  1106 of the 1702 rows), the same at every node after it and at the image queue sink.
- The viewfinder on the screen is still black. The picture is lost after the image queue, on the display side: backboardd
  makes a `fmt 500` (YCbCr8_420_2P) texture over the sink's surface. That is the next cause.
- The preview pipeline is built only when cameracaptured has a Metal device: `cam_metal=1` on the line (camera_metal.c).
  Without it the source node's preview output leads nowhere, scaler or not (the boot with `m2m=0` showed the same).
- The shutter still fails at once with -16808 (BWUBNode); the scaler changes nothing there.

### The display side: the picture reaches backboardd's GPU, CoreAnimation's draw of it is lost (24 November)

Phone 1, kernel built from main 72c0b593 and the fork 4285c9c8 (`cam_trace=1 cam_metal=1`), root as found plus a StarbiteMetal
with the diagnostics below (`YCbCrProbe.h`, `DrawNote.h`, all off unless set in `/var/local/mtl2-env`). Seven boots, D1 to D7.

- **Measured: the texture is right.** backboardd makes a `YCBCR8_420_2P` (500) texture over each of the image queue sink's
  surfaces and binds it in its main pass (render encoder, fragment slot 3). The probe (`SWEET_MTL2_YCCPROBE=1`) reads it on
  backboardd's own GPU: with the front camera, `read()`, a linear and a nearest sample in a compute function, and linear and
  nearest half-precision samples from fragment slot 3, all give RGB about 59 60 59 with 40 of 64 points lit, where the
  processor reads luma 59 with 40 of 64 lit. The 4:2:0 conversion, the surface's bytes as backboardd's GPU sees them, and
  sampling in either stage are right.
- **Measured: replacing the texture changes nothing.** With `SWEET_MTL2_YCCFLAT=1` every fragment binding of such a texture
  gets a BGRA8 copy of it instead (made by a compute pass on the caller's queue, committed just before the caller's command
  buffer). The viewfinder is still black. So the black does not come from the two-plane texture.
- **Measured: which draws show the camera** (pass log `SWEET_MTL2_PASSLOG`, draw notes `SWEET_MTL2_DRAWNOTE=1`). The first frame
  draws it with `TimgA3Xhf_Ixrg` straight into the framebuffer. Every later frame draws it with `TimgA3S1Xhfu_Ixrg` (four draws)
  into color 1, the pass's memoryless `RGBA16Float` attachment (load and store don't care), with the framebuffer's color 0
  masked off. `TimgBbcmA3Xhf_Isup` (twice) and `TbdsA3Xhf_Isup_Isrc` then write color 0. Samplers are clamped and normalized
  (nearest for the first draw, linear for the rest); texture coordinates are normalized (v from 178/1702). The camera's control
  blur also samples it (`TimgA2Xhfu_Ixrg` into 22 x 22 and 208 x 144, `downsample_4_frag_lph`). No refusal from backboardd while
  the viewfinder was up.
- **Inferred, not verified: where it is lost.** In the `S1` path the picture only reaches the screen through color 1, a
  memoryless attachment read back by a later draw of the same pass (framebuffer fetch). That is the next place to measure:
  what metal2vulkan does with that attachment and that fetch on PanVK (its rasterization-order and interlock features), and
  whether the pass is split into instances there. With the bridge's telemetry on (`touch /var/local/mtl2-telemetry`) through a
  camera session: no `new_instance_<reason>` count (the pass is not split), no degradation notice and no refusal from backboardd.
  So the next measurement is inside PanVK: how it keeps the memoryless color and serves its fetch.
- **Also measured:** with the camera open, backboardd runs at 495 to 498 of its 514 MiB GPU cap. The kernel refused it a
  24 MiB buffer after the photo (D5), and 13 times with the BGRA8 copies on (D7), where `mtl2.log` also has `OutOfMemory(Device)`
  refusals of textures and of two command buffers. Each converted 828 x 1702 texture keeps an `RGBA16Float` image of about
  11 MiB. Such refusals reach the console only while the process's console line budget lasts.
- **Also seen once, not explained:** both boots of D6, where the BGRA8 copy waited on the caller's thread for its own command
  buffer, stopped backboardd after its first textures (SpringBoard never drawn); D7, without the wait, booted normally.

### SpringBoard's main-display layout (18 November)

`camera_sbtrace.c` on phone 1 (dylib `-16`, `-17`) shows that SpringBoard does publish its layout: the home screen, the lock
screen and then `com.apple.camera` as elements, and the backlight's state 2 (on) once it is on. Two publishers carry it. The
one FrontBoard's shell makes for the main display gets the elements and the backlight state, flushes once at 23 s (no
elements, backlight 0) and never again. The second, made by SpringBoard and set as the primary publisher at 43 s
(`setPrimaryPublisher` in SpringBoard's log), flushes every change with its elements but without the backlight. A third,
for the continuity display, is held by a suppression (`startListener`, `-[SBContinuityDisplayLayoutCoordinator start]`) that
never ends, which is harmless. cameracaptured's monitor timed out waiting for the first layout at 21 s, before SpringBoard
published anything, and afterwards sees no application in front: `camera_layout.c`'s stand-in is still what lets the
camera run. Why the first publisher stops flushing (the shell's `%@ Loading` transition, `FBSystemShellInitialize`, ends
at 55 s with the other milestones, so it is not that) is not known yet.

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
