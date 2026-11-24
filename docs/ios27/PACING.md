# iOS 27 on the Galaxy S9: what holds a home-screen flick near 30 a second (24 November 2026)

Phone 2 (iOS 27.2.1, 24B156), fork kernel `scion/xnu-12377` f523de65 with the charger guard, the owner's root. Every number here
was measured on the phone with the console flicks of `msr.sh` (20 page flicks 1.2 s apart, then the Settings list and
open/close), seven minutes after SpringBoard started (after the slow mode). M = measured, I = inferred.

## 1. The answer

The "30 fps" of a flick is two different things, and neither is the clock of the big cores:

1. **About half of the frames of every flick are a SpringBoard animation that asks for 30 a second.** Around each page slide
   SpringBoard runs small animations whose frame rate range prefers 30 (seen: `SBFFluidBehaviorSettings` 20..120 preferred 30,
   a display link 20..120 preferred 30). CoreAnimation draws them every second refresh exactly: its render thread wakes at
   every refresh and goes back to sleep in 20 to 60 us on every other one; the frame itself is 4 to 12 GPU chains, 2 to 3 ms of
   GPU, 3 ms of render, nothing held (M). This is Apple's own choice, made the same way on an iPhone; it is left as it is. It
   is why the old yardstick (frames per 5-s window) sticks near 30 whatever is changed.
2. **The page movement itself is GPU-bound.** SpringBoard sets the page's offset 53 times a second (p50 16.7 ms apart), but
   the window server shows 35 to 37 of those frames a second: a heavy frame (40 to 86 chains) costs 25 to 30 ms of GPU, and
   two fragment passes are about 18 ms of it (one of 11.5 to 12.9 ms, one of 6.4 to 7.6 ms) (M). The render thread's CPU
   encode, 12 to 16 ms a heavy frame, overlaps the GPU's work on the frame before (M).

Not the cause (M): the vsync source (CoreAnimation's render thread wakes every 16.7 ms; the panel refreshes at 60 Hz, the
DECON's frame counter moves by one each refresh), the present path (present 1.8 ms; SwapEnd holds p50 0 ms), a frame-rate
limit we report (CoreAnimation renders at every refresh when it has something new), the kernel's GPU queue (10 us of idle
between two waiting chains), GPU waits by the render thread (it makes none: all of them, about 30 a second, are by the bridge's completion
thread `metal2vulkan-queue-wait`).

## 2. A frame, end to end (trace T5/T6/T10, home-screen flicks, fast mode)

| Stage | Where it is measured | Light frame (the 30-a-second animation) | Heavy frame (page moving) |
|---|---|---|---|
| SpringBoard moves the page | `sbpage.m`: `setContentOffset:` | - | every 16.7 ms (p50), 52.8 a second |
| Render thread wakes (CoreAnimation's display timer) | `cawake.m`: `display.primary` run loop | every 16.7 ms; every other wake ends in 20-60 us | never sleeps: late |
| Render (wake to SwapEnd call) | ring `S` | 3.1 ms | 12-25 ms |
| First chain queued | ring `B` | wake + 2.8 ms | in a burst just before SwapEnd |
| SwapEnd held (for the frame before's chains) | ring `S` to `R` | 0 | 5-18.5 ms |
| GPU work of the frame | ring `B`-`E`, `sgu_dump` | 2-5 ms, 4-14 chains | 25-30 ms, 40-86 chains; f 11.5-12.9 ms + f 6.4-7.6 ms + ~30 small |
| Our tick takes it, waits for its chains | ring `F`, `G` | next tick, nothing to wait | up to 25-35 ms |
| Present (GPU scale to 1440x2960) | ring `G` to `P` | 1.8 ms | 1.8-2.6 ms |
| On the panel | DECON frame counter at `P` (`dpu_probe=1`) | +2 refreshes each frame | +1 to +3 |

Swap-to-present p50 15 ms. In refreshes between swaps (runs of a flick, T5 / T6 / T10): heavy 1.79 / 1.72 / 1.76 (33.5 /
34.8 / 34.1 a second), light 2.0 exactly.

## 3. The proofs

- **GPU clock** (T7, `stargpu=455000`, the GPU 20 % slower, a lower level of Samsung's table): heavy frames 1.79 -> 1.90
  refreshes (33.5 -> 31.7 a second), light frames unchanged at 2; flicks 28.5-29.6 -> 26.8 fps on the old yardstick, the
  Settings list 20.9-22.7 -> 18.9 (M). The GPU is a stage the movement depends on; the light frames do not depend on it.
- **CPU clock** (bigcore-1124 F1066, the big cores at 1066 MHz): flicks 28.5 fps, unchanged on the old yardstick (M). With
  half of that yardstick fixed at 30 by the animation above and the movement GPU-bound, that is what both models predict (I).
- **Light frames' cadence**: the render thread wakes at every refresh with nothing new on every other one (M), while the
  requests seen in SpringBoard ask for 30 (M). A test that raises those requests to 60 (`sbpage.m` with `pd_raise=1`) is built
  and not run: it would show the light frames' cadence moving and the heavy frames not (the prediction).

## 4. What would make the movement 60

The GPU time of a heavy frame has to fall from 25-30 ms to under 16.7 ms, and two fragment passes are most of it:
1. Name the two passes (which render target, size, format, shader, blend and samples): the bridge's pass log
   (`tools/ios27/metal2/PassLog.h`) during one flick, matched to `sgu_dump`'s long chains by order.
2. Usual suspects for an 11.5 ms full-screen fragment pass on a Mali-G72 MP18 at 572 MHz with the memory bus at 1794 MHz:
   a large texture minified without mipmaps (the wallpaper), a blur done with many taps at full size, an extended-range
   (RGBA16Float, `SWEET_MTL2_DEXR=1`) target read and written several times, no AFBC on CoreAnimation's own targets.
3. Then the render thread's 12-16 ms of encode becomes the next limit at ~60-80 a second.

## 5. Tools

- Fork (`agent/pacing/frame-trace`): `kern.hakios.sweetfb_trace_go=S` starts the framebuffer's event ring from the USB
  console (6144 events, printed S seconds later with the first event's mach time); `P` carries the DECON's frame counter with
  `dpu_probe=1`; GPU waits by thread (`W`/`X`); `sgu_dump=S` also starts from it and prints 480 chains with the GPU's idle time
  before each. All off by default.
- `tools/ios27/perf/fbt-timeline.py LOG`: the ring, `cawake` and `sbpage` lines as one timeline, per-stage quantiles, swaps by
  size per flick, the page movement's own rate, the GPU waits by thread.
- `tools/ios27/perf/frame-observers/` (diagnostics, never in a root): `cawake.m` for backboardd (its CoreAnimation threads' run
  loops), `sbpage.m` for SpringBoard (the page offset, its main run loop, the frame rate requests it makes).
