# iOS 27 on the Galaxy S9: what the compositor draws wrong, and why (17 November 2026)

The owner's report on 17 November, on the first test phone with CoreAnimation's Metal renderer as the compositor
([LIQUID-GLASS.md](LIQUID-GLASS.md)): "multitasking menu rendering is broken, wallpaper is black". This page takes that
report and the project's other open rendering problems ([INSTALL-S9.md](INSTALL-S9.md) "Known problems",
[UI-NIGHT-1114.md](UI-NIGHT-1114.md), [METAL.md](METAL.md)) one at a time: what is drawn, what should be, the cause with
how sure it is, what was changed, and what a phone must show to settle it. A cause is called **certain** only where a
console line or a picture shows it, **likely** where the evidence points at it without showing it, a **guess** otherwise.

The evidence is the test phones' console logs and screenshots of 9 to 17 November, read on the build machine; none of it
is in this repository. The 17 November boots of the first phone ran the Metal compositor (`sbgl=0`, `mtl2-clients`
"backboardd SBRendererService"), the bundle of 14 November (the frontend of [LIQUID-GLASS.md](LIQUID-GLASS.md) with
PanVK's null-vertex-shader fix) and `mtl2-env` `SWEET_MTL2_DEXR=1 SWEET_MTL2_FLUSH=2 STARBITE_DRM_LEAKS=0`, which is what the
root build's step `80-metal-ca.sh` writes. Nothing here was run on a phone by this work; the experiments that decide each
point are at the end.

## The defects

| What is seen | Compositor | Cause | How sure | Changed |
|---|---|---|---|---|
| App switcher: the cards are blank white, only the app's icon and name above each | Metal (OpenGL ES: no picture yet) | the snapshot behind each card is drawn into an extended-range IOSurface, and the plug-in refuses that command buffer | refusal: certain; that it is the card's picture: likely | `frontend-dexr-surfaces.diff` |
| App switcher: the background is black | Metal (OpenGL ES: no picture yet) | there is no wallpaper for the switcher to show: PosterBoard has no poster, and the stop-gap image is put on two other windows only | likely | nothing (see below) |
| Home screen and lock screen wallpaper black | both | no poster ([METAL.md](METAL.md), [GPU-PATH.md](GPU-PATH.md)); with the stop-gap image installed both are drawn | certain | nothing new |
| Blur black (Control Centre, dock, folders) | Metal, without `SWEET_MTL2_DEXR=1` | metal2vulkan refuses CoreAnimation's variable-blur downsample, which samples an extended-range texture through a mip-filtered sampler | certain | none new: `SWEET_MTL2_DEXR=1` (on in the root build) is the answer and is drawn on 17 November |
| Lock screen clock an empty box | Metal, earlier configurations | the clock's glass goes through the same blur | guess | not reproduced on 17 November: the clock is drawn |
| No-copy buffers refused, 10 to 30 a boot in backboardd | Metal | the kernel's IMPORT_HOST cannot wire the memory, PanVK's import fails, metal2vulkan answers `NotRepresentable` | refusal: certain; what is lost with it: not known | `frontend-nocopy-diag.diff` (a line describing the memory) |
| A start-up command buffer refused at `generateMipmapsForTexture` | Metal | metal2vulkan has no mipmap generation | certain | nothing: seen once a boot, on CoreAnimation's start-up textures, with no visible effect known |

### The app switcher's cards

**Seen.** A screenshot of the switcher on 17 November (Settings, Games and a third app open): a black screen, three white
cards stacked, the icon and name of each above it, nothing of the apps' own pictures. A frame taken while the switcher opened
shows the white cards over a blurred home screen.

**The console.** The owner swiped up from the bottom edge of Settings at 18:03:17 (the touch from (799, 2948) to (813, 2563)
in panel pixels) and then swiped cards away. The plug-in's lines at that moment, in this order:

    native-metal: texture memory fmt 554 usage 0x5 storage 0 type 2 207x426 mips 1
    native-metal: command buffer refused: setRenderPipelineState: render pipeline: Invalid(Invalid { call: "set_render_pipeline_state", problem: Draw(AttachmentFormat(Color(0))) })

Format 554 is BGR10_XR (extended-range sRGB, 10 bits a channel in 32 bits); the texture is a render target over an IOSurface
("memory", storage 0) a quarter of the 828 x 1702 canvas in each direction. The same pair is in the boot before it (one
refusal at 17:54:47, when the switcher was opened, and one at 18:01:16) and in the first phone's own `mtl2.log` at 207 x 448
(the quarter of the 1792-row canvas). Each of those boots has one or two, the first at the moment the switcher opened. Boots
of 10 and 12 November (two phones) show the same refusal right after a full-size 828 x 1792 BGR10_XR surface target was made,
up to eight times a boot.

**Why it is refused (certain).** `SWEET_MTL2_DEXR=1` ([METAL.md](METAL.md), `patches/frontend-dexr.diff`) holds the
extended-range formats (552 to 555) of textures that live in device memory as RGBA16Float, so that the blur can sample
them, and it rewrites **every** render pipeline's color attachments of those formats to RGBA16Float to match. A texture
over an IOSurface or a buffer cannot be held so (its bytes are the surface's) and keeps format 554. A pipeline made for a
BGR10_XR target is then an 8-byte RGBA16Float color drawn into a 4-byte BGR10_XR attachment, which metal2vulkan refuses
(`render.rs`, `check_pipeline`: a placed color whose format differs and has no view of the attachment's size), and a
refusal drops the whole command buffer.

**What the refused buffer drew (likely).** The quarter-size extended-range surface made the moment an app leaves the
screen, once per app, is the reduced snapshot the switcher shows on the app's card; its command buffer is dropped, so the
card has nothing to show. Not seen directly: the switcher's snapshot files were not looked at.

**Changed.** `tools/ios27/metal/patches/frontend-dexr-surfaces.diff` (applied by `mk-bundle27.sh` after the four
before it): a render encoder notes which of its pass's color textures kept an extended-range format; when a pipeline whose
colors `SWEET_MTL2_DEXR` rewrote is set in such a pass, the plug-in uses the same pipeline made again from its descriptor with
those colors as the descriptor asked (made at the first such draw, kept on the pipeline, one compile, which the pipeline
cache keeps). Device-memory targets keep the RGBA16Float pipelines, so the blur stays as it is. Without
`SWEET_MTL2_DEXR=1` nothing changes (no pipeline is rewritten, so none is made again). The console line the first eight
times: `native-metal: render pipeline made again with colors 0x1 in their extended-range format (a pass over a surface)`;
a refusal of the remade pipeline is a `native-metal: render pipeline with colors ... refused` line and the command buffer is
refused as before.

What it does not do: a pass that mixes a held and an unheld extended-range color is drawn with each as its own texture
holds it (the mask is per color, so that works); a pipeline that gets functions added after it was made
(`addRenderPipelineFunctionsWithDescriptor`) is made again without them (not seen with CoreAnimation).

### The switcher's black background

**Seen.** The same screenshots: everything around the cards is black, where iOS shows the wallpaper behind the switcher.

**Likely cause: there is no wallpaper.** PosterBoard has never made a poster on this port (no poster extension is started;
the wallpaper picker fails, [UI-NIGHT-1114.md](UI-NIGHT-1114.md)), so the windows that should hold the wallpaper hold nothing.
What the home and lock screens show is the stop-gap of `tools/ios27/metal/sbwall.m`: an image added as a layer to
SpringBoard's `SBHomeScreenWindow` (under the icons) and `_SBWallpaperSecureWindow` (the lock screen), the two windows of its
default list; its console lines of 17 November list fourteen SpringBoard windows and name those two. The switcher is
`SBMainSwitcherWindow`; while it is up the home screen window is not shown, and nothing under the cards has the image. A
frame taken while the switcher opened showed a blurred home screen behind the cards, so blur itself works there; the black
comes once the switcher has settled.

**Not changed.** Putting the stop-gap image under the switcher window too (`/private/var/local/sbwallpaper-windows` =
`Wallpaper:top HomeScreen:bottom MainSwitcher:bottom`, no code) would be a fallback, not a fix: the switcher window also
carries apps as they open and close, so the image could hide the home screen during those animations. It is experiment E4
below, to run only after E1 says the background is black under OpenGL ES as well. The fix is a real poster.

### Blur, and the lock screen's clock

The blur of iOS 27's materials is drawn by CoreAnimation's variable-blur shaders. Without `SWEET_MTL2_DEXR=1` each command
buffer that uses the downsample is refused:
`indexed draw: Unsupported(... feature: Format, needs: NotImplemented ... variable_blur_clamp_downsample_frag_lph ...)`
(54 lines in three logs of 9 and 10 November, before the variable existed or with it off) and once the glass background
(`glass_background_cr_lph`, two lines in a rotation test of 12 November with it off). With `SWEET_MTL2_DEXR=1`, which the
root build has written since then, none of these lines is in any log and Control Centre's blur is drawn (screenshot of 17
November). The empty clock box of [INSTALL-S9.md](INSTALL-S9.md) was not seen on 17 November (the lock screen showed the
time); that it came from the same refusal is a guess, which E3 tests.

### No-copy buffers

Every Metal boot logs 10 to 30 `no-copy buffer refused: ... HostMemory, needs: NotRepresentable` in backboardd (1,791 lines in
60 logs). metal2vulkan imports the caller's pages through `VK_EXT_external_memory_host`, which PanVK implements with
StarbiteMali's IMPORT_HOST (`iokit/Kernel/arm/StarbiteMali.cpp`, `importHost()`: wire for writing, else for reading, else
refuse). The answer `NotRepresentable` comes back when the import is refused, so these are ranges the kernel could wire
neither way ([GPU-PATH.md](GPU-PATH.md) saw the same for some of backboardd's image memory, for which `SweetSurface.cpp`
copies instead). The texture shapes made just before them are small RGBA8 images (19 x 46, 28 x 68, 31 x 23). What
CoreAnimation does with a nil buffer (a copy of its own, or nothing drawn) is not known, and no picture so far shows
something missing that these explain. `frontend-nocopy-diag.diff` adds one line for each of the first 16 refusals with
the VM region the pointer lies in (protection, maximum protection, sharing mode, the region's tag), so that the next boot
says what memory it is. No fix was made: copying the bytes would make a buffer that no longer aliases the caller's memory,
which is wrong for any caller that writes it afterwards.

## The refusals, all logs

Every distinct refusal of the Metal plug-in in the test phones' logs of 9 to 17 November (114 logs; the plug-in prints only
the first 64 problem lines of a process, so the counts are a floor):

| Refusal | Lines | Logs | Feature that uses it | Status |
|---|---|---|---|---|
| no-copy buffer: `HostMemory`, `NotRepresentable` | 1,791 (and about 14,600 more in the trace lines of five stress runs) | 60 | images given to the render server as memory (likely) | diagnostic added |
| texture over IOSurface: `BufferTexture(RowPitch ...)` | 180 | 3 | the display surfaces asked for turned (landscape) | fixed 12 November ([METAL-ROTATION.md](METAL-ROTATION.md)) |
| `variable_blur_clamp_downsample_frag_lph`: `Format`, `NotImplemented` | 54 | 3 | the blur of every material | answered by `SWEET_MTL2_DEXR=1` |
| `setRenderPipelineState`: `AttachmentFormat(Color(0))` | 46 | 10 | drawing into BGR10_XR surfaces: the switcher's card snapshots (likely), full-size ones on 10 and 12 November | `frontend-dexr-surfaces.diff` |
| `generateMipmapsForTexture`: `MipmapGeneration`, `NotImplemented` | 4 | 4 | CoreAnimation's start-up textures (776 x 24, 10 levels) | none needed so far |
| `glass_background_cr_lph`: `Format`, `NotImplemented` | 2 | 1 | glass, with `SWEET_MTL2_DEXR` off | answered by `SWEET_MTL2_DEXR=1` |
| texture: `OutOfMemory(Device)` | 3 | 2 | GPU memory at the cap | [UI-NIGHT-1114.md](UI-NIGHT-1114.md) |

## What the plug-in does not implement, and what each costs

- **Sampling an extended-range texture with mipmaps or two filters** (metal2vulkan holds BGR10_XR and BGRA10_XR as packed
  integers): blur black. `SWEET_MTL2_DEXR=1` holds device-memory ones as RGBA16Float; a surface-backed one cannot be held
  so, and is drawn into through a pipeline of its own format (`frontend-dexr-surfaces.diff`); sampled with mipmaps it would
  still be refused (not seen).
- **Host-memory import that the kernel cannot wire**: a nil no-copy buffer (cost above, not known).
- **Mipmap generation** (`generateMipmapsForTexture`): the command buffer is refused; seen only at start-up.
- **Device-side**: PanVK on the Mali-G72 (Bifrost v6) is marked broken upstream and is opened with
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` and `MESA_VK_VERSION_OVERRIDE=1.2` (`HotMetal2.mm`). Mesa 26.2.6's PanVK reports for
  this architecture no geometry or tessellation shaders, no `multiDrawIndirect`, no `shaderFloat16` (half-precision shaders
  run at 32 bits) and no `shaderClipDistance`, and does report independent blending, dual-source blending, logic ops,
  `fragmentStoresAndAtomics`, `VK_EXT_external_memory_host` and timeline semaphores (`panvk_vX_physical_device.c`).
  No refusal in the logs names a missing device feature; all of them are the runtime's own gaps above.

## The kernel's read-only GPU mapping (xnu draft PR 29) and these paths

The fork's draft PR 29 maps client pages that the client may only read (IMPORT_HOST's fallback to wiring for reading, and
surfaces `SweetSurface.cpp` wrapped `kIODirectionOut`) read-only for the GPU, and frees a closed buffer's pages only after
the chains that could reach it. For the paths studied here, nothing has the GPU write memory the client mapped read-only:

- the switcher's snapshots and the display's swap surfaces are surfaces the kernel allocated (written by the GPU, writable);
- the wallpaper (the stop-gap image, or a poster's) and the icons reach the render server as images it samples; a wrapped
  surface over memory that cannot be wired for writing (an image mapped from a file) is only sampled, and memory that cannot
  be wired at all is copied (`sweet_surface_wrap_memory()`), into a kernel buffer that is writable;
- blur and glass work in device memory of the plug-in's own;
- RenderBox's drawables (`newTextureWithBytesNoCopy`) are render targets over memory their caller allocated writable, which
  wires for writing as before;
- the no-copy buffers refused above are refused before and after the change alike.

The one case the PR itself names is a range that spans writable and read-only pages: it is mapped read-only as a whole.
None was identified here; the PR's line `STARLTE GPU USER: a chain wrote GPU address ..., in N pages NAME[PID] imported
that it may only read` shows one if it happens, and the experiments below grep for it.

## Experiments for the phone

Each one is a boot or a bundle swap, and one picture decides it. In each: open Settings, swipe up from the bottom edge and
hold (the switcher), take a screenshot; then a screenshot of the lock screen and of Control Centre. Lines to look for:
`AttachmentFormat`, `fmt 554`, `made again with colors`, `no-copy memory`, `variable_blur`, `STARLTE GPU USER`.

| | What | Outcome and meaning |
|---|---|---|
| E1 | the OpenGL ES compositor, nothing else changed | cards with the app's picture: the card defect is the Metal path's; background black too: the background is the missing wallpaper |
| E2 | the bundle with `frontend-dexr-surfaces.diff` (Metal compositor) | `made again with colors 0x1` and no `AttachmentFormat`, cards with the app's picture: fixed; cards still white without a refusal: the snapshot is lost elsewhere |
| E3 | the old bundle, `SWEET_MTL2_DEXR=0` (control) | cards drawn, `variable_blur` refusals, blur black, clock box empty: both what the variable is for and its flaw confirmed |
| E4 | only if E1's background is black: `sbwallpaper-windows` with `MainSwitcher:bottom` | image behind the cards: the cause is the missing poster; icons vanishing while an app opens or closes: that fallback is not acceptable |

## Not done

- The switcher's background needs a poster: the wallpaper picker ([UI-NIGHT-1114.md](UI-NIGHT-1114.md)) is the way to one.
- What the no-copy refusals cost (the diagnostic line, then a picture).
- Mipmap generation in the plug-in (none needed so far).
- Nothing on this page was run on a phone.
