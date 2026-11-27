# iOS 27 on the Galaxy S9: what the display takes as a layer (the camera viewfinder, 24 November 2026)

The camera viewfinder was black, even though the picture was right everywhere up to the display. QuartzCore handed the
preview to our framebuffer as a display layer of its own, a `'420f'` (YCbCr 4:2:0, two planes) surface. `SwapEnd`
draws 4-byte BGRA layers only, so that layer was never drawn. The framebuffer now says what it takes, and QuartzCore
composites the preview on the GPU into its BGRA layer. Kernel side: Scion-Project/xnu PR 70 (draft),
`iokit/Kernel/arm/SweetFramebuffer.cpp`.

## What QuartzCore reads

`CA::WindowServer::AppleDisplay::update_display_limits` (QuartzCore, iOS 27) reads these keys from the framebuffer
service. Without them it assumes the display can do everything in the first four rows.

| Key | Absent means | We publish | Why |
|---|---|---|---|
| `IOMFBYUVSupportKey` | YCbCr layers taken | `false` | `present()` draws BGRA only |
| `IOMFBYUVPackedUnSupported` | packed YCbCr taken | `true` | same |
| `IOMFBYUV420UnSupported` | 4:2:0 YCbCr taken | `true` | same: the camera's `'420f'` |
| `IOMFBRGBMultiPlaneSupportKey` | multi-plane RGB taken | `false` | same |
| `IOMFBNumLayers` `{MaxNumLayers}` | several layers | (not published) | upper BGRA layers are blended |
| `IOMFBSupportsRotation` | no rotation | (not published) | none is done |
| `IOMFBBlendDisabledKey` | blending on | (not published) | upper layers are blended (premultiplied) |

(Read in code: the flags start as `(w & 0x2d81) | 0x903c`. Bit 0x8 is YCbCr, 0x10 packed, 0x20 4:2:0, 0x4 multi-plane
RGB, 0x2000 several layers, 0x4000 rotation.)

## Measured

Boot argument `sweetfb_layerlog=1` (2 also times a CPU conversion) describes every swap that enables more than one layer.

- **Before** (phone 1, camera open): each swap has 3 layers, mask 0x7. Layer 0 is empty. Layer 1 is CA's BGRA surface
  (828 x 1702). Layer 2 is a camera sink `'420f'` surface: 828 x 1702, Y bpr 832, CbCr 414 x 851, source and
  destination full screen, no transform. The kernel screenshot is exactly 0 over the viewfinder.
- A CPU conversion of layer 2 to the panel (1440 x 2960, nearest, BT.601) takes **38.5 ms a frame**. That is more than
  two refreshes before the UI is blended over it, and it would run in the present. So presenting the layer ourselves
  was not taken. A DECON YUV window would be the native way; our display path has none today.
- **After**: no multi-layer swaps during the camera. The kernel screenshot shows the preview. Rear camera on the desk:
  viewfinder mean RGB 22,19,22, 100 % non-zero. Front camera: a lit room, mean RGB 110,109,101.

## Not done

- **GPU memory with the camera.** With the default caps, backboardd reaches its per-client GPU buffer cap
  (`499 of 514 MiB`) with the preview composited. The bridge then crashes on `VK_ERROR_OUT_OF_DEVICE_MEMORY` when the
  Camera goes to the background. Underneath, cameracaptured's GPU buffers (with `cam_metal=1`) are not released after a
  camera session, with or without this change: wired GPU buffers went 355 to 615 MiB after one session, and 368, 542,
  749, then 1025 MiB (the all-clients cap) over three sessions.
- **Picture quality** (measured from the console, read in `userland/src/camera_isp.c`). The preview is made one RGB
  pixel per 2 x 2 Bayer cell: rear 1008 x 756, front 816 x 612. That is blown up to 1504 x 1128, turned and scaled to
  828 x 1106, then scaled to the 1440-wide panel, so the panel shows about 756 (rear) or 612 (front) real pixels
  across. There is no colour tuning on the root (`no colour tuning for the s5khm2sd`: identity colour matrix, no lens
  shading). The lens is not driven: `cam_af` is not on the boot line.
- **Rotation.** The M2M scaler's Transform convention matches Apple's: VideoToolbox's `vt_Rotate_Celeste` passes
  FlipVertical as bit 0, FlipHorizontal as bit 1 and CW 90 as bit 4, with flips after the turn, as
  `VTPixelRotationProperties.h` says. A wrong orientation therefore comes from the frames' own orientation (the kernel
  turns the front sensor's frames half round), not from the scaler.
