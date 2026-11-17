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
