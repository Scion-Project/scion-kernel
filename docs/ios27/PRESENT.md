# iOS 27 on the Galaxy S9: the present without the copy (17 November 2026)

Under the Metal compositor a frame ends with a present: a GPU job that scales CoreAnimation's 828 x 1702 swap surface to
the panel's 1440 x 2960 in the buffer the display controller reads. It costs 1.93 to 2.10 ms of every frame on the GPU
(phone 1, 17 November: `STARLTE GPU PRESENT: N frames, average 1711 to 2093 us`, max 2.5 to 2.9 ms), and the DECON then
reads the 17.0 MB result at every refresh. The 16.5 line pays nothing of it: the DPP scales the window server's surface
(`fb_dpp=1`) and scans it out with nothing copied (`fb_zc=1`). This page is the iOS 27 side of the same thing, in two
stages that can each be switched on alone:

- **Stage A, the present by the DPP** (`sweetfb_dpp=1`): built (fork branch `agent/scion-present/present`), off by
  default, **not run on a phone**.
- **Stage B, zero-copy** (`sweetfb_zc=1`, reserved): designed, **not built**. One fact about iOS 27's CoreAnimation is
  missing for it to meet the standard below; Stage A's boot measures it.

The standard: no tearing, no surface scanned out while it can be drawn into, no stale frame shown as current, and every
failure back to today's path by itself, reversible at run time.

## 1. Both sides as they are

### 16.5 (Scion's own code, the reference)

- **The DECON as S-Boot leaves it** (`hakios.exynos_dpu.c`, its header; scion-starlte `README.md`, run 33 of 2
  November): DECON0 at 0x16030000 in MIPI command mode, frames started by the panel's TE (HW trigger), one window on
  (window 5) on DMA channel 5 (block G0), whose IDMA reads the 1440 x 2960 boot framebuffer at 0xCC000000, 5760 bytes a
  row, through the DPP and DSC to DSIM0. The DECON reads that buffer once every refresh.
- **`fb_dpp=1`**: the window is moved to channel 1 (VGF0), one of the two DPPs with a scaler. VGF0's IDMA is set up from
  the boot channel's format and bus settings (the format field of `IN_CON`, `OUT_CON`, `DEADLOCK_NUM`, `BUS_CON`,
  `DYNAMIC_GATING_EN`, `IN_REQ_DEST`), the source size (stride in pixels, height), the image size and the base address
  (`IN_BASE_ADDR_Y/C`, 32 bits: buffers below 4 GB, aligned to 256 bytes here); the DPP gets the boot channel's format
  (ARGB8888, per-pixel alpha bit as the boot's), the image size, the scaled size (the window's, from `WIN_END`), the
  ratios (source / destination in 12.20) and Catmull-Rom coefficients (9 phases, 8 taps across, 4 down, in 1/512), and
  the start positions. Its registers are forced in (`SFR_UPDATE_FORCE`: VGF0 is idle), then `DATA_PATH_CONTROL_1` moves
  the window to channel 1 and a shadow update is asked for (`SHADOW_REG_UPDATE_REQ`, all windows and the global bit);
  the DECON takes it at the start of its next frame and clears the request. After three refreshes the IDMA's and DPP's
  configuration-error and IRQ error bits are read: any error and the window goes back (`hakios_scanout_detach()`).
- **The flip**: a frame is drawn into one of three buffers that is neither shown nor waiting; its address goes to VGF0's
  `IN_BASE_ADDR_Y/C` and a shadow update is asked for; the DECON latches it at its next frame (TE). The next flip is asked
  for only once that one is taken (`SHADOW_REG_UPDATE_REQ` reads 0). A flip not taken in 100 ms gives the scan-out up.
- **Formats** (Samsung's `dpp_reg.c` `dpp_reg_set_format`, `dpp_drv.c`, `dpp.h`; facts only): the IDMA takes 8888 in four
  orders with or without alpha, RGB565, the four 2-10-10-10 orders (`IDMA_IMG_FORMAT_*2101010`, DPP type ARGB8101010),
  and YUV; the RGB sizes have no alignment (multiples of 1), image 16 to 4096, source width (stride) up to 65534, scaling
  1/2x to 8x, VGF0/VGF1 only. Nothing takes 16-bit floating point or 64-bit extended-range pixels.
- **`fb_zc=1`** (`MiniIOMobileFramebuffer.cpp` and `MiniIOSurface.cpp`, branch `scion/xnu-8796`): backboardd's
  display-sized one-plane 'BGRA' surfaces are made of the DPU driver's pool (`hakios_scanout_carve()`: physically
  contiguous 12 MB slots below 4 GB outside XNU's RAM, mapped write-combined); a frame whose only layer is such a surface
  over the whole display is shown by pointing VGF0 at it (`hakios_scanout_show()`), after the flip before has been
  taken. Lifetime: the surface is held (its use count raised) from its flip until another one's flip is taken; SwapWait
  is answered only once the swap it names is on the panel (`fb_swapwait=1`, forced on with `fb_zc`), which is what iOS
  16's CoreAnimation waits on before it draws into the other surface again. Measured on 4 November with SwapWait
  answered at once: the surface on the panel changed before the next flip in 69 of 1683 sampled frames and between that
  flip being asked and taken in 438 more; with `fb_swapwait=1`, 0 and 4 of 1745. Any other frame (more layers, a part of
  the display, ordinary memory, the surface on the panel drawn into again) is copied as `fb_dpp` does. (`fb_zc_hold` 2
  shows a frame drawn into the surface already on the panel as it is, which the standard here does not accept.)
- **Seen on a phone**: both defaults on the S9 since 2 and 3 November (scion-starlte `device.conf`: "frames scaled by the
  DPP's VGF0 and flipped at the DECON's refresh (fb_dpp=1), the window server's own surface scanned out with nothing
  copied (fb_zc=1)").

### iOS 27 today (fork `scion/xnu-12377`)

- **The boot chain** is S-Boot, then Starbite's shim, then XNU. S-Boot sets the display up on both lines; the fork's
  `pexpert/pexpert/arm64/A5XELTE.h` (STARLTE) says S-Boot leaves DECON_F (0x16030000) scanning out 0xCC000000, 1440 x
  2960, stride 5760. **No iOS 27 boot has read the DECON yet**: the fork has the DPU service
  (`bsd/dev/hakios.exynos_dpu.c`, ported, compiled for STARLTE and FRANKEL, `optional scion_generic_arm64`) with no
  caller but the read-only probe of `dpu_probe=1` (`iokit/bsddev/IOKitBSDInit.cpp:842`), and no phone log on the Mac
  mini has its lines. (`profiles/ios-27.copy` lists only the Pixel 10's `hakios.lga_dpu.c`, the frame kicker of its
  command-mode panel; the S9's DPU service lives in the fork.) Its carve-out on this path is 0xD8000000-0xDFFFFFFF
  (`bsd/sys/hakios.exynos9810_mem.h`): scan-out buffers 0xD8000000-0xDAFFFFFF, `fb_zc`'s pool 0xDB000000-0xDDFFFFFF (four
  12 MB slots), the camera 0xDE000000-0xDFFFFFFF. The extra RAM the ADT gives the VM (`extra_ram=1 extra_ram_mask=127`
  on phone 1) ends at 0xD8000000; `carve_out_is_free()` refuses every buffer if any page of the carve-out is the VM's.
- **The swap surfaces** (phone 1's e1 boot of 17 November, `SWEET FB: swap ... layer 0` and `STARLTE GPU PRESENT:
  surface ...`): two in turn, surfaces 1 and 3, each 828 x 1702, format 0x42475241 ('BGRA', 8 bits a channel), 3328
  bytes a row, 1384 pages (5.66 MB). Surface 1 is the framebuffer's default surface (`GetLayerDefaultSurface`), a
  kernel `IOBufferMemoryDescriptor` made by `sweet_surface_create_buffer()` (`SweetSurface.cpp`, page-sized pieces,
  cached; its first page was at 0x886a37000, above 4 GB); surface 3 is backboardd's own (IOSurface create, selector 0;
  its first page at 0xc8610000, in the extra RAM). Neither is physically contiguous, and surface 1 is above 4 GB: the
  IDMA cannot read either. The 10-bit extended-range formats of `METAL.md` (BGRA10_XR, held as RGBA16Float with
  `SWEET_MTL2_DEXR=1`) are CoreAnimation's intermediate textures, not the swap surfaces.
- **The present** (`SweetFramebuffer.cpp`): a swap only records the frame (`swap()`, with the GPU queue's last chain
  number, and holds the window server until the chains of the swap before are done); the 60 Hz timer (`vsyncFired()`)
  takes the newest one, sleeps until its own chains are done, and `present()` → `gpuPresent()` maps the surface into one
  of four 12 MiB GPU slots, cleans it if a CPU may have written it (`sweetspeed&524288`), and
  `starlte_gpu_present_draw()` (`bsd/dev/starlte_gpu.c`) runs the present blob (a tiler job and a fragment job built
  offline by `tools/ios27/gpu/starlte-g72-tri.c`: a bilinear textured quad) into the boot framebuffer at GPU VA
  0x04000000, under the GPU's lock, flushing the L2 at both ends. It writes the buffer the DECON is reading: it can tear.
  `sweetfb_scale=1 sweetfb_gpu=1 sweetfb_lw=828 sweetfb_lh=1702` is the S9's line; 828 x 1702 has the panel's shape
  exactly (828 x 2960 / 1702 = 1440.0), so the quad fills the panel without bars.
- **SwapWait** (selector 6) is called by iOS 27's backboardd with three scalars and answered at once (the `default:` of
  `externalMethod()`); what they mean and whether CoreAnimation waits on it before it draws into a surface again is not
  known. The relbuf-info notification (type 7, `sweetfb_relbuf=1`, off) exists and is sent after a swap is presented
  or replaced.
- **The Pixel 10** (FRANKEL) has none of this: everything below is under `ARM64_BOARD_CONFIG_STARLTE`.

## 2. Stage A: the present by the DPP (`sweetfb_dpp=1`, built)

**What changes.** The present job no longer scales: it copies the frame one pixel for one into one of three 828 x 1702
buffers (3328 bytes a row, 5.66 MB, 0x570000 apart) of the scan-out carve-out at 0xD8000000, 0xD8570000, 0xD8AE0000.
VGF0's DPP scales that buffer to the panel. The flips are 16.5's: a buffer is drawn only when it is neither shown nor
waiting to be shown; a flip is asked for only once the one before has been taken at a refresh; frames reach the panel
at most once a refresh and nothing is drawn into what the DECON reads (today's tearing goes). The swap surface stays
where it is (no change to `SweetSurface.cpp` or to what CoreAnimation sees).

**The copy job** is the present blob retargeted (`starlte_gpu_present_copy()`): nine words rewritten for a render target
of the frame's size, each checked to hold the panel's value first (a blob of another layout is refused and today's path
runs): the fragment job's tile bounds (0x124), the framebuffer's size and bounds (0x220, 0x228), the render target's base
and row stride (0x2a0, 0x2a8), the tiler context's framebuffer size (0x30c), the scissor (0x49c), and the quad, its
texel coordinates and the texture's size as the canvas patch writes them. The tiler's hierarchy mask (0xff) is the same
for both sizes (Mesa's `pan_select_tiler_hierarchy_mask()`: 185 and 107 bins on the larger side, eight levels either
way). With one texel a pixel and unnormalised coordinates, the bilinear fetch at a pixel's centre is that texel. The
three buffers are mapped into AS0 once, at GPU VA 0x18000000, with the level-3 tables of the last two surface slots
(the surfaces keep slots 0 and 1, which is what two swap surfaces need; a third would be remapped as today).

**The sequence of a frame** (the vsync thread):
1. the first time: `hakios_scanout_probe()` (the DECON as expected), `hakios_scanout_memory()` (three buffers; refused
   if any page of the carve-out is the VM's), the GPU and CPU mappings;
2. if the flip asked for last has been taken, that buffer is the front;
3. the copy job into the buffer that is neither front nor pending (always one of three);
4. the first time, `hakios_scanout_attach(2, ...)`: VGF0 set up to read that buffer and scale 828 x 1702 to the window
   (1440 x 2960: 1.739 both ways, inside 1/2 to 8), the window moved, three refreshes, the error bits read;
5. otherwise: if a flip is still pending, wait (1 ms polls, up to 100 ms) until it is taken, then
   `hakios_scanout_flip()` to the new buffer.

**Every way it can fail, and what the screen shows:**

| Failure | Seen as | What the screen shows | Back to |
|---|---|---|---|
| DECON not as expected (not one window, not 0xCC000000, video mode) | `exynos_dpu: ... left alone`, `A5XELTE FB dpp: not set up: the DECON is not as expected` | today's picture: nothing was written | today's path for the boot |
| the carve-out is the VM's | `exynos_dpu: memory: ... is the VM's RAM`, `not set up: no buffers` | today's | the same |
| the blob not as expected | `dpp: the present blob is not laid out as expected` | today's | given up |
| the copy job faults | `STARLTE GPU PRESENT: dpp copy ... FAILED` | the frame drawn by today's job into the boot framebuffer, then the DECON moved back to it | given up |
| VGF0 refused or reporting errors | `exynos_dpu: VGF0 cannot take it` / `VGF0 after N refreshes: ... errors` / `attach: ... not taken` | the DPU driver already went back to the boot channel; the frame is drawn there by today's job | given up |
| a flip not taken in 100 ms | `dpp: back to the boot framebuffer (a flip not taken in 100 ms)` | the frame drawn into the boot framebuffer, the window back on it | given up |
| a frame it cannot take: more layers, a part of the display, another size | `dpp: back to the boot framebuffer (a frame of ...)` | the shown buffer drawn scaled into the boot framebuffer first, the frame over it, then the window moved back: whole frames only | again after 120 frames it can take, 16 times a boot |
| the display off (`RequestPowerChange(0)`) | `dpp: back to the boot framebuffer (the display off)` | the same, then today's fade | again at the next frame |
| `sysctl kern.hakios.sweetfb_dpp=0` on the USB console | the sysctl's line, then `back to the boot framebuffer (switched off ...)` | the same, at the next tick | `=1` asks again, also after a give-up |

If the GPU cannot draw the shown buffer into the boot framebuffer when leaving, the CPU's scaled copy does it (slow,
once). While the DPP shows the frames, what is drawn into the boot framebuffer is not seen: the kernel's text console
(muted under the GUI anyway with `sweetfb_noconsole=1`) and a panic's screen. The USB console's `shot` reads the buffer
VGF0 shows (`hakios_fb_scanned()` returns 2, the buffer, 828 x 1702 at 3328 bytes a row: the real picture before the
DPP's scaling; `tools/usb-shot.py` takes any size).

**What it saves, the arithmetic.** A frame is 828 x 1702 x 4 = 5.64 MB of pixels; the panel 1440 x 2960 x 4 = 17.05 MB.
- Today's job reads 5.64 MB and writes 17.05 MB (22.7 MB) and shades 4.26 million pixels. Stage A's reads 5.64 and
  writes 5.64 (11.3 MB, half) and shades 1.41 million (a third).
- Today's 2.0 ms (average of the s3a and e1 windows: 1.93 to 2.10) has a fixed part (the 2.2 KB blob written to uncached
  memory, two job starts polled at 10 us, four L2 flushes) and a part that grows with the work. With a fixed part of 0.2
  to 0.3 ms and the rest scaling with the bytes (the passes on this GPU look bandwidth-bound, PERFORMANCE.md §2.1):
  0.25 + 1.75 x 11.3 / 22.7 = **about 1.1 ms**, so **about 0.9 ms saved a frame** (with the fragments' third instead of
  the bytes' half: 0.25 + 1.75 / 3.02 = 0.83 ms, 1.2 ms saved). PERFORMANCE.md's estimate was 1.1 ms; the range is 0.8
  to 1.2 ms.
- The DECON reads 5.66 MB a refresh instead of 17.05: 0.34 GB/s at 60 Hz instead of 1.02, 0.68 GB/s of DRAM bandwidth
  the GPU's passes no longer share (2.4 % of LPDDR4X's 28.7 GB/s peak). Not counted above.
- What it does not save: the job itself (about 1 ms), its turn on the GPU between the app's chains, the clean of a
  surface a CPU wrote, the wait for the frame's own chains. And a frame that is ready while the flip before has not been
  taken waits for that refresh (counted in the line below); today's present would draw it at once, into the buffer being
  read.

**The counters** (`A5XELTE FB dpp:` in the framebuffer's ten-second line, beside `A5XELTE FB gaps`):
on / given up (why) / off (last left: why); frames by the DPP, by the GPU's scaled copy, by the CPU; flips; how many
found the flip before not yet taken and the wait (average, max); the DECON's refreshes (FRAME_COUNT); times left and
attached; SwapWait calls (for the last swap, the one before, other); the swap surfaces seen (ids, up to four) and how
often a swap named the same surface as the swap before. The copy's time is measured as today's present is (the two
jobs, under the lock): `STARLTE GPU PRESENT: dpp copy: N frames, average X us, max Y us` every 600 frames, against
today's `STARLTE GPU PRESENT: N frames, average X us`. (`sweetfb_prof`'s "present avg" includes the wait for the flip
before under Stage A.)

## 3. Stage B: zero-copy (designed, not built)

**What it would do.** backboardd's two swap surfaces made of `fb_zc`'s pool (`hakios_scanout_carve()`, 0xDB000000 up:
5.66 MB in a 12 MB slot, contiguous, below 4 GB, outside the VM), the default surface (`sweet_surface_create_buffer()`)
too, and a frame whose only layer is one of them over the whole display shown by `hakios_scanout_show()` at the next
refresh, with no GPU job at all. Everything else as Stage A (whose buffers are the fallback for any frame that does not
qualify).

**What qualifies, as far as the kernel can tell:**
- format: the swap surfaces are 'BGRA' 8-bit (seen), which the IDMA takes as the boot channel's format; no conversion.
  (The DPP has no format for CoreAnimation's 64-bit extended-range textures, but those are not swap surfaces.)
- contiguity and address: only if made of the pool (a `SweetSurface.cpp` change: backboardd's display-sized one-plane
  'BGRA' creates and the default surface, as 16.5's `MiniIOSurfaceScanout()` does). StarbiteMali imports a surface by
  its physical pages whatever its memory (`importSurface()`, `getPhysicalSegment()`), so the GPU can draw into a pool
  surface as into any other; it maps an imported surface cached for the client (`kIOMapCopybackCache`), which the
  frontend's cleans handle as today. The DPP reads DRAM; the GPU's L2 is flushed at the end of every chain.
- alignment: base 256 bytes, rows a multiple of 4 bytes and at least the width (3328 for 828: yes).
- how the kernel learns which surface is current: the SwapEnd record's layer-0 surface id (as today).

**The rule that must hold:** a surface is on the panel from the refresh that takes its flip until the refresh that
takes the next one, and nothing may draw into it then. With two surfaces in turn, frame N+2 is drawn into frame N's
surface, so its drawing may only start once frame N+1's flip has been taken.

**What is missing.** The kernel cannot see which surface a GPU chain writes: a StarbiteMali submit lists buffers with no
word of which are read and which written, and a chain reaches buffers it does not list (`StarbiteMali.cpp`, the
comment at `GEM_CLOSE`). So the rule has to come from CoreAnimation's own protocol, and **what iOS 27's CoreAnimation
waits on before it draws into a swap surface again is not known**: on iOS 16 it was SwapWait (16.5's measurement
above); on iOS 27 backboardd calls SwapWait with three scalars (meaning unknown), and its IOMFB server also paces on
the relbuf-info notification (type 7) when `SupportRelbufInfoCb` is published. Nor is it known that it never names the
same surface in two swaps in a row (a frame drawn into the surface the panel shows). Stage A's line counts both (SwapWait
calls and whether their first scalar is the last swap's id; the surfaces in turn and the repeats), and the first 12
SwapWait calls are logged with their scalars.

**The design once the fact is known** (all off unless `sweetfb_zc=1` with `sweetfb_dpp=1`):
1. surfaces from the pool as above; a create that finds no free slot gets ordinary memory (and its frames are copied);
2. a frame qualifies when its only layer is a pool surface over the whole display, 828 x 1702 'BGRA', and that surface
   is neither the front nor the pending one; otherwise Stage A copies it;
3. the release: whichever of SwapWait (answered once its swap is on the panel, two refreshes at most, as 16.5) or relbuf
   (sent for swap N only when the refresh that takes swap N+1's flip has come) the measurement shows CoreAnimation
   waits on; the surface held (its use count raised, as 16.5's `fb_zc_hold` 1) from its flip until the next flip is
   taken;
4. the guard: a SwapEnd that names the surface on the panel, or the one a flip waits to show, means CoreAnimation drew
   into it while it could be read: that frame is copied (Stage A), and zero-copy given up for the boot with
   `zc: given up (CoreAnimation drew into the surface on the panel)`. This is after the fact, so it is a guard against
   a second torn frame, not a way to accept one; zero-copy is not switched on by default until a phone run with the
   release in place shows 0 of these in a few thousand frames, as 16.5's did;
5. the kernel's other choice, should iOS 27 wait on nothing usable: hold every window-server chain queued after
   SwapEnd(N+1) in the GPU queue until swap N's surface has left the panel. That needs no help from CoreAnimation and
   cannot deadlock (the release depends only on swap N+1's chains, queued before the hold, the timer and the DECON),
   but it does not cover a frame drawn into the surface that is already on the panel (the chains of swap N+1 itself),
   which only the guard above sees, and it takes the GPU's overlap of one frame with the next away whenever the GPU is
   ahead of the panel. Not recommended before the measurement.

**What it would save.** The whole job: 1.93 to 2.10 ms a frame (no fixed part left), plus the same 0.68 GB/s of DECON
reading as Stage A (5.66 MB a refresh), and the GPU's lock no longer taken by the vsync thread between the app's chains.
PERFORMANCE.md's 2.0 to 2.4 ms (the 13 November frame's 2.4 ms present included).

## 4. The phone test (one boot each; phone 1, the s3a line plus `sweetspeed` + 16384 for the gaps)

Every boot: an idle minute on the home screen, then the same 300 s of console swipes as the E-series of
17 November. The kernel is the fork branch `agent/scion-present/present` (STARLTE).

| Boot | Arguments added | Read | Pass |
|---|---|---|---|
| P0 | `dpu_probe=1` | `exynos_dpu: DECON0: control ... (command mode, running), trigger ... (HW on)`, `windows ... (1 on)`, `window N ... on channel N (G0)`, the channel reading 0xcc000000 at 1440x2960, `probe: as expected`; no `is the VM's RAM` line | the probe as expected and the carve-out free (nothing is written) |
| P1 | `sweetfb_dpp=1` (with P0's argument) | `A5XELTE FB dpp: 3 buffers of 828x1702 ...`, `STARLTE GPU PRESENT: dpp: 4167 pages at 0xd8000000 mapped`, `dpp copy 1 ... ok`, `exynos_dpu: VGF0 scaler: 828x1702 to 1440x2960`, `VGF0 after 3 refreshes: ... errors 0 ... 0`, `A5XELTE FB dpp: VGF0 scales 828x1702 ...`; then every ten seconds `A5XELTE FB dpp: on, VGF0 scales; frames by the DPP N, by the GPU's scaled copy 0 ...`, `STARLTE GPU PRESENT: dpp copy: N frames, average X us` | no `given up`, no `back to the boot framebuffer` while swiping; the picture right (below) |
| P2 | P1, and in the middle of the load `sysctl kern.hakios.sweetfb_dpp=0`, 30 s later `=1` | the sysctl's lines, `back to the boot framebuffer (switched off ...)`, frames by the GPU's scaled copy; then `VGF0 scales` again | the switch both ways, the picture whole at each |
| P3 | P1, then the power key (display off), 10 s, on again | `back to the boot framebuffer (the display off)`, the fade, then `VGF0 scales` at the first frame | the screen black while off, the picture back on |

**What to compare** with phone 1's baseline boots of 17 November (e1-ref and s3a, same load): the present's time (`dpp copy:
average` against today's 1711 to 2093 us; expected 0.8 to 1.2 ms), the gaps (`A5XELTE FB gaps`: share of 1s, motion
fps), chains per second and the queue's time per chain (`STARLTE GPU QUEUE`), and Stage A's own counters: "found the
flip before not yet taken" should be a small share of the flips (each is a frame shown a refresh later than today's would
have started to be drawn); DECON refreshes should be about 600 per ten seconds. And, for stage B, the SwapWait and swap
surface counts (and the 12 logged SwapWait lines): whether the first scalar is the last swap's id, how many SwapWaits per
swap, how many surfaces in turn, and whether the same surface ever comes twice in a row.

**The picture.** The USB console's `shot` under Stage A is VGF0's buffer, the frame before the DPP's scaling (828 x 1702,
`usb: shot: fb_dpp: VGF0's buffer N`): it shows whether the copy is right (compare with a shot of the same screen in a
boot without the argument, scaled), not whether the scaler is. The scaler's output only reaches the panel: **a photograph
of the screen** (by the owner or a camera pointed at it) is the check for it: the home screen sharp and in its colours,
filling the panel with no bars, no black or green band, no tearing during a swipe (a fast swipe photographed at 1/250 s
or shorter). Today's present can tear; Stage A should not.

**If the screen goes wrong** (black, frozen, garbled): on the USB console, `sysctl kern.hakios.sweetfb_dpp=0`. The next
refresh tick draws the last picture into the boot framebuffer and moves the window back to the boot loader's channel
(`back to the boot framebuffer (switched off ...)`). If the console does not answer, a reboot without `sweetfb_dpp=1`
is today's path exactly.

## 5. Not done

- Stage B (above): its missing fact first, from P1's counters.
- The vsync timer still free-runs; under Stage A it could follow the DECON's FRAME_COUNT (a tick per refresh) so that a
  frame never waits for a flip it just missed. Not built: the counters will say if the waits matter.
- A panic under Stage A draws into the boot framebuffer, which is not shown: the panic is in the log, not on the screen.
