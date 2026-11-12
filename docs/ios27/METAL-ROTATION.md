# iOS 27 on the Galaxy S9: the interface on its side under the Metal compositor (12 November 2026)

With CoreAnimation's Metal renderer as the compositor ([LIQUID-GLASS.md](LIQUID-GLASS.md)) the panel kept its last upright
picture whenever the interface turned to landscape, until it was upright again. The app underneath was running and took
touches. Under the OpenGL ES compositor the same turn drew.

## What CoreAnimation asks for

The display's two swap surfaces never change: 828 x 1792 pixels, 3328 bytes a row, rows along the panel. When the display
is to show the interface turned by a quarter, CoreAnimation's Metal renderer does not turn its own drawing. It asks Metal
for a texture over each swap surface whose descriptor has the turned size, 1792 x 828, and a rotation (a private property
of Metal's texture descriptor; 3 on a phone held in landscape, 0 otherwise), and then draws the interface into that
texture the way it reads. A GPU with turned render targets stores the picture turned, so the surface still holds what the
panel scans out. (The OpenGL ES renderer has no such texture and turns its drawing itself, which is why that path drew.)

Seen on the phone, in a line the plug-in printed for the refusal: `want 1792x828 fmt 80 usage 0x5 rotation 3 | surface
828x1792 bpr 3328`, asked from the renderer's "texture of this surface" step when it makes a buffer current, sixty times in
the two seconds of the turn. The plug-in ignored the rotation, took the request for a texture 1792 pixels wide over rows
of 3328 bytes, and refused it (`BufferTexture(RowPitch { bytes_per_row: 3328, row_bytes: 7168 })`). With no texture for
its destination CoreAnimation drew nothing.

## What the plug-in does now

No Vulkan image can lie over the surface that way, so a texture asked for with a rotation is two textures
(`tools/ios27/metal/patches/frontend-turned.diff`, "turned textures"):

- the one the caller gets has the asked shape and lives in device memory. Everything drawn into it, sampled from it or
  copied with it is as the caller means it; its `iosurface` and `rotation` answer what was asked;
- the surface's own texture, unturned, which the caller never sees.

Every command buffer that drew into the first (or into a view of it) ends with one more render pass, added at commit: a
single triangle over the surface's texture whose fragment reads the picture through the turn
(`tools/ios27/metal2/TurnShaders.metal`, one fragment function for each of the three turns, nearest sampling, texel for
texel). The surface is written before the buffer completes, which is before the window server hands it to the display.
The pass is built by the bundle's script with the Metal compiler of the same Xcode and compiled on the phone like any
other pipeline (and kept in the pipeline cache).

Cost: one pass over the screen for each frame while the interface is turned, and 6 MiB of device memory for each of the
two swap surfaces. Upright, nothing changes: a descriptor without a rotation takes the path it always took.

Only rotation 3 has been asked for by iOS 27 and only that one has been seen on a panel; 1 and 2 are written the same
way and are untested. A texture with a rotation that is not a plain 2D, single-level, single-sample texture of the
surface's turned size is refused with a line.

## Seen

Phone held on its long edge, Notes in front: landscape, then upright (the accelerometer answered by the console's
`sensors fake 0 4096 0`), then landscape again, each drawn, with the glass of the search field and the round buttons that
the OpenGL ES compositor does not draw. `native-metal: turned texture 1792x828 fmt 80 turn 3 over surface 1 (828x1792)`
once for each swap surface, no refusal. The picture is the same way up as the OpenGL ES compositor's.
