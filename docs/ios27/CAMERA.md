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
