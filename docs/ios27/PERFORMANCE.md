# iOS 27 on the Galaxy S9: where a frame's time goes, and what would make it 60 a second (17 November 2026)

The yardstick is the owner's: "smoother, it must be smoother, 60fps". The test is a swipe of the home screen under the Metal
compositor, which draws Liquid Glass. A swipe frame has 16.7 ms. This page is in four parts:
- the cost map: where the time goes, by component, and how sure each number is;
- what has to come out of a frame to hold 16.7 ms with margin;
- the changes ranked by expected gain per unit of risk;
- the experiments for a phone, one boot each.

Nothing new here has run on a phone. Two changes come with this page, both off by default: the honest fences of the shim
(scion-kernel #327) and `perfmap=1` / `sgu_pri=` in the fork (xnu #33, branch `agent/scion-perf/perfmap`).

**How sure.** Each number carries one of four tags: **M** measured (where), **I** inferred from measurements (how),
**G** a guess (why), **N** nobody has measured it. The sources:
- `OPTIMISE.md`, `SPEED.md`, `NIGHT.md` (9 November), `LIQUID-GLASS.md` (9 to 13 November), `EIGHT-CORES.md`, `AOT-REWRITE.md`;
- the console log of phone 1's boot "s3a" of 17 November, today's kernel with `tmu=3`. Only numbers are taken from it. Its load was 219 console
  swipes in five minutes, plus about two minutes of swipes before that, with the M3s at 1794 MHz and the GPU at 572 MHz throughout;
- Samsung's device tree for this phone (`exynos9810-starlte_eur_open_26.dts`), read for facts.

## 1. Two frames

There are two measured snapshots, and they do not agree. The difference is itself the first finding.

**(a) The heavy glass frame of 13 November** (`LIQUID-GLASS.md`: the first phone, a full home screen):
- 35 job chains at 0.42 to 0.48 ms is **14.7 ms** of GPU, plus the present's **2.4 ms**: **17.1 ms** against 16.7 (M);
- 43 to 47 % of swipe frames on time, 37 fps in motion (M);
- 95 % on time while a list scrolled in an app (M).

**(b) The s3a swipe of 17 November** (phone 1, today's kernel):
- 24 to 25 frames a second presented, every 5 to 7 s window the same (M: `A5XELTE FB`);
- 3,500 to 3,950 chains per 10 s, i.e. about 375 a second, about 15 a frame (M: `STARLTE GPU QUEUE`; I: 375 / 25);
- an average chain of **1.55 to 1.69 ms**, the longest 15.6 to 16.2 ms in every 10 s window (M);
- the GPU 54 to 64 % busy, chains queued 20 to 22 ms on average, at most 40 to 42 deep (M);
- the present 1.93 to 2.10 ms (M);
- backboardd 47 to 53 % of a core, SpringBoard 37 to 45 %, kernel_task 9 to 10 %, the widget renderer 6 % (M: `STARLTE TOP`);
- the GPU user client counted **664 waits per 10 s at 9.2 ms** each, 6.1 s of waiting per 10 s, summed over threads (M: `STARLTE GPU CALLS`).

**(c) The 13 November kernel on the same phone, root and load** (phone 1's boot "e0-old", 17 November):
- 13.6 to 14.6 frames a second (M);
- 4,580 to 5,140 chains per 10 s, averaging 1.31 to 1.37 ms, the longest **32.1 to 32.6 ms** in every window (M);
- chains queued 63 to 69 ms on average, up to 135 deep (M);
- 338 to 350 waits per 10 s at 18.9 to 19.5 ms (M).

So today's kernel is the better of the two on this content (25 against 14 a second, chains queued 21 against 66 ms). The
13 November figures of (a) were on a lighter load: a scripted 300 ms drag every 2 s, where these runs swipe back to back.
The longest chain is about 32 ms on one kernel and about 16 ms on the other: two refresh periods, then one. Something tied
to the display's cadence, not a pass's drawing, is a candidate for it (§2.4).

What (b) says, step by step:
- 15 chains × 1.6 ms is 24 ms of GPU work a frame, plus 2.1 ms of present: 26 ms (I).
- A frame took 40 ms (25 a second), so the GPU stood idle about 14 ms of every frame: 60 % busy matches 26 / 40 (I).
- With pipelining, the next frame's chains would be queued while this frame's run, and the frame would take max(CPU, GPU) ≈ 26 ms.
  It takes about CPU + GPU instead. The fence waits explain that (section 2.2) (I).
- The chains are 3.5 times longer than on 13 November, and the longest takes a whole refresh (16 ms). That has to be explained
  before anything else is tuned (section 2.4). It could be the content, a slow shader or format path, or a slower memory bus.
  Which one is N.

## 2. The cost map

### 2.1 The GPU itself

| Item | Value | Tag, source |
|---|---|---|
| G3D clock | 572 MHz from 6 s after start to the end of the run; 260 MHz before | M: `STARLTE GPU: clock 260000 -> 572000 kHz`, no `the G3D at` line (the G3D never reached 85 C: 61 C at most) |
| G3D levels | Samsung: 572 546 455 338 299 260 MHz, `gpu_max_clock_limit` 572000; whether the APM's own table has a level above 572 is N | the device tree; `perfmap=1` prints the APM's table |
| Memory bus while the GPU is at 572 | Samsung's `gpu_dvfs_table` asks for MIF at 1794000 or more at 546 and 572 MHz (its fifth column, which the Mali driver reads as `mem_freq`: `mali_kbase_platform.c`); nothing on this tree asks for any MIF rate, so it is whatever S-Boot left. That rate is **N** | the device tree; `perfmap=1` reads PLL_MIF and the APM's MIF table |
| What a full-screen pass costs in DRAM traffic | one 828×1702 BGRA8 target is 5.6 MB; written once and read once by the next pass, 11.3 MB. LPDDR4X at 1794 MHz on four 16-bit channels gives at most 28.7 GB/s, so 0.39 ms at the theoretical peak and about 0.6 ms at a realistic 65 % | I (arithmetic). It matches the 0.42 to 0.48 ms per chain of 13 November too well to ignore: **the chains may be bandwidth-bound, not overhead-bound** |
| Fixed cost of a chain (start, end found, flushes) | `LIQUID-GLASS.md` says "mostly not drawing". No measurement separates it from the drawing | N. `sgu_prof=1` gives the time by chain kind and the GPU's idle time between chains; `sgu_irq=1` gives when the end was signalled |
| Chains a frame | 35 heavy (13 Nov), 25 bench (9 Nov), about 15 (17 Nov s3a) | M, M, I |
| Formats | extended-range textures (BGRA10_XR) held as RGBA16Float (`SWEET_MTL2_DEXR=1`): 8 bytes a pixel instead of 4 for whatever CoreAnimation draws in XR | M (the setting is on phone 1); the share of passes in XR is N |
| AFBC (Arm frame buffer compression) | PanVK 26 uses it for optimal-tiled images that are not storage, host-copy or mutable-format (`panvk_image_can_use_afbc`), and the kernel answers AFBC_FEATURES 0 (supported). IOSurface-backed targets are linear, so no AFBC | I (code). Which of CoreAnimation's targets get AFBC is N |
| Two job slots at once | +4 % frames shown, measured once (12 Nov), with wrong tiles | M, `LIQUID-GLASS.md` |

### 2.2 The kernel's GPU path

| Item | Value | Tag, source |
|---|---|---|
| Submit | 2 to 3 us (queue on) | M: `STARLTE GPU CALLS`, submit 3,764 × 2 us |
| Queue | one chain at a time on the GPU, in order; the worker looks every 10 us for 200 us, then naps 50 us at a time; about 52,000 naps per 10 s | M (s3a) |
| GPU idle between two waiting chains | N | `sgu_prof=1` reports it |
| Waits | **664 per 10 s, 9.2 ms each**. Every fence wait of the shim is SBM_WAIT_IDLE, a wait for every chain queued at that moment, so a wait for frame N also waits for frame N+1's chains (`tools/ios27/mesa-ios/starbite-drm/starbite_drm.c`, `drmSyncobjWait`, before #327). Which threads wait (render thread, completion thread) is N | M (count, time); I (cause, from the code); `sbm_prof=1` names the threads |
| PanVK's fence transfer | PanVK moves its queue's fence into the caller's fences and semaphores through a sync file (`DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` / `FD_TO_HANDLE`). The shim answers those with ENOSYS, so the caller's fence is never really attached and only the whole-queue wait makes it work | I (Mesa 26.2.6 `jm/panvk_vX_gpu_queue.c` `panvk_queue_transfer_sync`, the shim's `drmIoctl`) |
| Cache maintenance | the frontend cleans a surface only when its seed moved (`SWEET_MTL2_FLUSH=2`), the present only when a CPU wrote it (`sweetspeed&524288`) | M: both are on phone 1's line and in its `mtl2-env` |
| Present | a GPU job (tiler and fragment) that scales the 828×1702 frame to the panel's 1440×2960 under the GPU's lock: **1.93 to 2.10 ms** a frame, in turn with the app's chains. It reads 5.6 MB and writes 17.0 MB; the display controller then reads that 17.0 MB buffer at every refresh, 1.02 GB/s at 60 Hz whether anything changed or not | M (time); I (bytes) |
| Tearing | the present draws into the buffer the DECON is reading | I (`LIQUID-GLASS.md`) |

### 2.3 The CPU side

| Item | Value | Tag, source |
|---|---|---|
| backboardd (CoreAnimation's render server) | 47 to 53 % of a core while swiping (s3a); 0.19 to 0.25 on 9 November with the Metal compositor; 8.9 ms a frame on the GL compositor | M, M, M (`OPTIMISE.md`) |
| SpringBoard | 37 to 45 % of a core while swiping. At 25 frames a second that is about 16 ms of CPU per frame, summed over its threads; how much of it is on its main thread is **N** (the profiler, `sweetspeed&16384`, would say) | M, I |
| Which cores | `sweetspeed&65536` recommends cpu0 and the four M3s; 85 to 90 % of backboardd's samples were on M3s once the FP16 forms were emulated | M (`OPTIMISE.md`, 9 Nov) |
| M3 clock | 1782 to 1794 MHz the whole run. `cpufreq_gov_big_min=1794000` is a built-in default, so the governor never lowers them; 1794 MHz is also their ceiling with all four online (Samsung's `quad_freq`, `policy-max`) | M (s3a `stat` lines: 1,014 to 1,022 ms of every second "at 1794") |
| A55 clock | 455 to 912 MHz average; the touch boost (1690 MHz) never fired in s3a because console swipes do not go through the touch screen's thread ("boost: 0 touch") | M. With a real finger it fires (N on iOS 27) |
| Traps | `STARLTE A64E: 0 emulated`; LSE on the M3s about 6,000 `ldop` per 10 s (one hot site outside the rewritten images); no FP16 pin lines | M (s3a). Cost: 600 traps/s × a few µs ≈ 0.2 % of a core (G) |
| Thermal | big 68 C at most under the swipe load, little 63, G3D 61; nothing throttled | M (s3a, from phone 1's notes) |

### 2.4 The unexplained 16 ms chain and the 1.6 ms average

Every 10 s window of s3a has a longest chain of 15.6 to 16.2 ms, and the average is 1.6 ms against 0.45 four days earlier.
- If one chain a frame is the 16 ms one, the other 14 average about 0.6 ms (I: 15 × 1.6 = 24; 24 - 16 = 8; 8 / 14 = 0.57).
  On that reading, one pass costs as much as the whole rest of the frame.
- Candidates, none measured:
  - a full-screen blur at full resolution with many taps;
  - a pass in RGBA16F (twice the bytes);
  - a shader that spills (the `bifrost-ra-multispill` patch exists because some do);
  - a texture sampled through a slow path;
  - the widget renderer's or SpringBoard's own Metal work (SBRendererService is a Metal client on phone 1);
  - a memory bus slower than the boot loader left it on 13 November.
- The 13 November kernel's longest is about 32 ms on the same content (§1 c): exactly two refreshes, against one today.
  So the longest "chain" may not be GPU work at all. A chain's time is from its start to the worker seeing its end, and the
  worker sees it only once it holds the GPU's lock, which the present also takes. Anything that holds that lock, or keeps the
  worker off a core, for a refresh would show the same way (I).
- `sgu_prof=1` (by pid, by kind, a histogram) and `sgu_dump=S` (208 chains in order with their microseconds; it needs the
  scripted input, `sweetspeed&4096`) will say which pass it is, or show that it is not a pass. This is experiment E1 and the
  first thing to do.

### 2.5 A second of the system, start-up and idle

| Item | Value | Tag |
|---|---|---|
| Idle CPU | 2 to 5 % at the lock screen (9 Nov); 13 % (s2, 17 Nov) | M |
| Start | kernel to SpringBoard's exec 5 s, first presented frame 22 s (s3a); 33 to 35 s from the reboot command on 9 November | M |
| Metal device at start | 4 to 6 s before PanVK's first call, nearly all `dlopen()` of 38 MB outside the cache; prewarming it into the page cache did not help | M (`LIQUID-GLASS.md`) |
| Pipelines | 98 creations, 8.3 s with an empty cache; 1.5 s with it warm | M |
| UFS read speed during launch | not measured | N |
| Page faults on first touch, the compressor | compressor region 1,407 MB, `STARLTE MEM: free 407 wired 462` (MB) during the run | M (values), N (fault rates) |
| GPU memory | about 312 MiB of buffers wired (s3a); about 430 MiB for backboardd at idle on 12 Nov, with the 514 MiB per-client cap reached after five minutes once | M |
| Daemons that spin | `rapportd` once at 137 % (recorded in the docs); none in s3a's top 8 | M |

## 3. What 60 a second needs

At 60 Hz a frame has 16.7 ms. "With margin" here means the GPU's frame, present included, at or under **14.5 ms** (87 %), so
that a frame 15 % heavier than average still fits. The CPU stage of the next frame must overlap the GPU's, which needs §4 item 1.

**The 13 November heavy frame: 17.1 ms. At least 2.6 ms has to come out of the GPU's work.**

| Change | Saves (estimate) | How the estimate is made |
|---|---|---|
| Present as an unscaled copy into an 828×1702 buffer that the DPP scales (`fb_dpp` as on 16.5) | 2.0 → about 0.9 ms: **-1.1 ms** | bytes 22.7 → 11.3 MB (half), and the fixed part (two jobs, the blob, the lock) stays; G |
| ... or zero-copy scan-out (`fb_zc` as on 16.5: the DPP reads the very surface CoreAnimation drew) | **-2.0 to -2.4 ms** | no GPU job at all; I |
| The job interrupt and flush reduction (`sgu_irq=1`, `sgu_flushred=1`) | -20 to -60 us a chain × 35 = **-0.7 to -2.1 ms** | a 50 us nap finds a chain's end 25 us late on average plus the wake; a start flush is a few to 30 us; G until E2/E3 |
| The second job slot | about -4 % of the frame, -0.6 ms | measured once (12 Nov, 54 against 52 frames a swipe step) |

- Present by DPP plus the interrupt and flush reduction: 17.1 - 1.1 - (0.7 to 2.1) = **13.9 to 15.3 ms**. On the edge.
- Zero-copy plus the same: **12.6 to 14.4 ms**. 60 a second, with margin at the better end.
- The rest, about 12 ms, is 35 passes of CoreAnimation's own work at roughly 0.35 ms each. That is what this GPU does with this
  renderer: Apple's GPUs keep a pass's intermediate in tile memory and compress every target, where here each pass makes a
  round trip through DRAM (§2.1). Only drawing fewer passes, or making each pass move fewer bytes, makes it smaller.

**The 17 November frame: 40 ms. Three things, in this order:**
1. **The serialisation.** With honest fences (§4 item 1) a frame should take max(CPU, GPU), about 26 ms, instead of the
   sum: 25 → about 38 a second (G). This holds if the waits are on the frame's critical path; `sbm_prof=1` says which threads wait.
2. **The 16 ms chain.** If it is a bug or a slow path, fixing it takes the GPU from about 26 to about 10 + 2 ms (G). Only
   then is 60 within reach for this content.
3. Then the items of the 13 November table above.

**The honest ceiling.** For the 13 November content, all of the above working gives about 12.6 to 14.4 ms of GPU a frame,
which is 60 a second if the CPU overlaps. For anything heavier, the inherent cost is about 0.35 ms per full-screen pass (the
DRAM round trip, §2.1).
- At 35 passes that is 12 ms; at 45 passes it is 16 ms, and 60 is out of reach without drawing less.
- What draws less without lying to the user is the owner's call: Reduce Transparency, Reduce Motion (§4 item 15).

## 4. The changes, ranked

Rank by expected user-visible gain per unit of risk. "Built" means compiled, not run on a phone.

1. **Honest fences** (scion-kernel #327, `STARBITE_DRM_SEQ=1` in `mtl2-env`).
   - **State**: built; tested on a Mac against a stand-in for the kernel's queue (2,400 random waits, none ended early).
     The Metal bundle needs a rebuild with it (`mk-panvk27.sh`, `mk-bundle27.sh`, which now makes its own source tree).
     The PanVK of phone 1's c2 bundle (`cc241e55`, 14 November) carries an earlier version of the same thing behind
     `STARBITE_DRM_FENCES=1` (its boots print `SBDRM[pid]: async submits on, fences of their own chain off`); the shim now
     has one implementation and takes either name.
   - **What changes**: a wait waits for its fence's own chain, not for every chain queued at that moment.
   - **Gain**: up to 25 → 38 a second on s3a's content (§3), and touch-to-picture shorter by up to a frame of GPU time (G).
   - **Risk**: a wait that ends too early would let PanVK reuse memory under the GPU. Everything the shim cannot follow exactly
     keeps the old whole-queue wait (no fence attached, timeline points, queries, unknown handles, old kernels).
   - **Verify**: `sbm_prof=1`. Read `STARLTE GPU WAITS` (whole queue against one chain, by thread), the shim's
     `drm[pid] fences 10 s:` line, frames and gaps; no `GPU USER: ... FAILED` / MMU fault line.
   - **Cost**: a bundle build.
2. **Find the 16 ms chain** (E1).
   - **State**: instruments exist (`sgu_prof=1`, `sgu_dump=S`, `sweetspeed&16384` for the gaps histogram).
   - **Gain**: possibly the largest single one (§2.4), unknown until measured.
   - **Risk**: none (counters only).
   - **Cost**: one boot.
3. **The job interrupt** (`sgu_irq=1`, fork `c51aa2ac`).
   - **State**: built, not run.
   - **Gain**: §3 table.
   - **Risk**: the line not attachable (S-Boot's GIC grouping), a lost interrupt (costs one sleep; after 16 the worker polls for the rest of the boot).
   - **Verify**: the attach line says 0 for INTID 314; the irq line has I as most chains, T close to 0, X well under 50 us.
4. **Flush reduction** (`sgu_flushred=1`, `0537327e`).
   - **State**: built, not run.
   - **Gain**: part of the §3 range.
   - **Risk**: low. This is what Linux's Panfrost does on this GPU, and only the flush at a chain's start is left out, when one has happened since its submit.
   - **Verify**: the per-chain time against E2.
5. **The present by the DPP, then zero-copy** ([PRESENT.md](PRESENT.md)).
   - **State**: stage A, the present by the DPP (`sweetfb_dpp=1`), built on the fork branch `agent/scion-present/present`,
     off by default, not run on a phone. Stage B, zero-copy, designed, not built: what iOS 27's CoreAnimation waits on
     before it draws into a swap surface again is not known, and stage A's boot counts what it needs (SwapWait's
     arguments, the swap surfaces in turn).
   - **What changes (A)**: the present job copies the 828x1702 frame one for one into one of three buffers of the
     display's carve-out (0xD8000000 up) instead of scaling it into the panel's; VGF0's DPP scales it; flips at the
     DECON's refresh, never into the buffer it reads (today's present can tear).
   - **Gain**: A: the job reads 5.6 MB and writes 5.6 instead of 17.0 (11.3 MB against 22.7) and shades a third of the
     pixels: about 2.0 -> 1.1 ms, **-0.8 to -1.2 ms** a frame; B: the whole 1.93 to 2.10 ms. Both: the DECON reads 5.7 MB a
     refresh instead of 17.0 (0.68 GB/s less). No tearing.
   - **Risk**: the DECON on the iOS 27 boot chain has never been read (`dpu_probe=1` is the first boot of the test); a
     wrong descriptor faults the copy job. Every failure goes back to today's present by itself (the table in
     PRESENT.md), and `sysctl kern.hakios.sweetfb_dpp=0` on the USB console goes back at once.
   - **Verify**: PRESENT.md §4: `A5XELTE FB dpp:` and `STARLTE GPU PRESENT: dpp copy: average N us` against today's
     present, the gaps, and a photograph of the screen.
6. **The memory bus** (`perfmap=1`, this page's fork branch).
   - **State**: built, not run.
   - **What it does**: reads PLL_MIF and the APM's MIF, INT and G3D tables.
   - **Gain**: if MIF sits below the 1794 MHz Samsung gives the GPU at 572 MHz, every bandwidth-bound pass is slower in
     proportion. A change of the MIF rate is not built: Samsung's kernel pairs it with a bus traffic-shaper scenario
     (`bts_update_scen`) and a MIF/INT constraint table sent to the APM at start (`exynos9810_bus_mif.c`). It would be a task of its own, and only if the read says it is worth one.
   - **Risk of the read**: one register never read on this phone before. Samsung reads it from the same exception level;
     if this boot loader had made it secure, the read would be an SError 60 s in. That is why it has its own argument.
7. **Queue depth** (`sgu_depth=N`).
   - **State**: built, not run.
   - **Gain**: latency, not frame rate. In s3a the queue never went past 42, so 48 changes nothing there. It mattered in the 95 % scroll run (depth 199, 55 ms queued).
   - **Try**: 32 (about two frames of 15 chains).
8. **Two M3s at 2314 MHz** (`gov_policy=3`, built and run on 16.5 only).
   - **What changes**: two M3s are parked (no longer recommended, still powered in WFI) and the other two run at 2314 MHz instead of 1794: +29 % clock for one thread.
   - **Cost**: half the big cores' parallel throughput.
   - **Facts**: Samsung allows 2314 MHz with one or two M3s online (`single_freq`/`dual_freq`) and 1794 with three or four. The APM's voltages are 856 mV at 1794 and 1012 mV at 2314 (s3a's table), 80 % more dynamic power per running core (I: (1012/856)² × 2314/1794 = 1.80). Two running instead of four is 0.90 of the cluster's dynamic power, plus the leakage of the two parked cores at the higher voltage. Samsung's own kernel runs above 1794 only with its over-current protector driven (`exynos-ocp`, `max-freq-wo-ocp = <1794000>`) and the other two cores powered off. This tree has neither. That is the risk: the symptom of an over-current would be a reset, not damage, but it is untested.
   - **Gain**: only where one thread is the limit. After honest fences, SpringBoard's or backboardd's main thread may be (§2.3).
   - **Verify**: `stat` "big ms" above 1794 non-zero, the temperatures every minute, the gaps.
   - **Whether to run 2314 without the OCP driver is the owner's call.** The owner's answer (17 November): not without it.
     `gov_policy=3` now parks as before but holds the M3s at 1794 MHz unless the over-current guard's stage two allows more
     (`OCP.md`, (3a)).
9. **The worker's priority** (`sgu_pri=92`, this page's fork branch).
   - **State**: built, not run.
   - **Gain**: small (G). It matters if a time-constrained render thread shares the worker's core.
   - **Verify**: `prof`'s idle time between chains, against the same boot without it.
10. **Fewer passes.**
    - **State**: not built; needs E1's dump first.
    - **What**: CoreAnimation's glass is a backdrop capture, a blur in several down- and up-sampled passes, and a composite. A pass that only clears, or a target re-created every frame, shows in the dump as a short fragment chain with nothing before it. The plug-in could keep targets and drop no-op passes.
    - **Gain**: N.
11. **The second slot** (`sgu_slots=2`): the interface is built; PanVK's side is not, nor is the cause of 12 November's tiles known. About 4 % (§2.1).
12. **The G3D above 572 MHz**.
    - **State**: 16.5's `mali_max` adds a level only if the APM's table has one at 950 mV or less. Whether this phone's APM has one is N (`perfmap=1` says).
    - **Gain**: +X % on shader-bound passes, nothing on bandwidth-bound ones.
    - **Decision**: beyond Samsung's limit, so the owner's call; the heat limits (85 and 95 C) stay as they are.
13. **The A55s.** The floor is 455 MHz, and the touch boost only follows the touch screen's thread. Threads pinned to an A55 by FP16 vector code (CoreGraphics) run there at whatever the A55s run at. `sweetspeed&131072` (a 1456 MHz floor) measured no change on 9 November; nothing to do now.
14. **AFBC**. `PANVK_DEBUG=noafbc` in `mtl2-env` is an A/B of what it buys today (a measurement, not a change).
15. **What iOS can be told to draw less of** (owner's call, not applied):
    - **Reduce Transparency**: the glass's backdrop blur becomes an opaque fill. That removes the blur passes, which are most of a glass frame's chains (G: 35 → 15 or fewer).
    - **Reduce Motion**: parallax and zoom become cross-fades. Fewer frames in which everything moves.
    - Neither hides anything from the user; both change how iOS looks.
16. **Rejected**: mapping extended-range textures to a 4-byte format to halve their bytes (RGB10A2 clamps colours outside 0..1:
    a wrong picture); skipping a flush or a clean that is needed; racing the GPU.

**What the 16.5 line shows the same hardware doing.** On 16.5 the present is the DPP's (`fb_dpp=1`), the scan-out zero-copy
(`fb_zc=1`), the GPU's clock follows its load (`mali_gov=1`), and the cores' governor is policy 1, all by default
(scion-starlte `device.conf`). Its compositor is OpenGL ES through Panfrost, with no Liquid Glass. Its swipe frame rate is
not recorded in these docs, so it is not a number to compare with.

## 5. Experiments for the phone, in order (one boot each)

The base is phone 1's s3a line: today's kernel, `tmu=3`, the Metal compositor. Every boot gets an idle minute on the home
screen first, then the same 300 s of console swipes. Record the temperatures every minute and which screen was in front.
`sbm_prof=1` is on from E1.

| # | Arguments added | Read | Decision |
|---|---|---|---|
| E1 | `sgu_prof=1 sbm_prof=1 sgu_dump=5`, `sweetspeed` + 16384 + 4096 (gaps histogram, scripted input for the dump) | `STARLTE GPU QUEUE prof` (kinds, histogram, idle between chains, pids); `STARLTE GPU DUMP` (where the 16 ms chain is, and what comes before it); `STARLTE GPU WAITS` (which threads wait); `A5XELTE FB gaps`; TOP | the 16 ms chain's process and kind; whether the waits are the render thread's. If fragment chains average over 1 ms, the time is fill and bandwidth: E6 before E2 |
| E2 | E1 + `sgu_irq=1` | the attach line (0 for INTID 314), the irq line (I most, T about 0, X well under 50 us), prof's averages and idle time, gaps | attach failed or T large: report and stop; else keep it |
| E3 | E2 + `sgu_flushred=1` | time per chain against E2 | keep if not slower |
| E4 | best + `sgu_pri=92` (fork branch `agent/scion-perf/perfmap`) | idle between chains | keep if it falls |
| E5 | best + `sgu_depth=32` | `queued avg`, gaps | for touch latency; keep if frames do not drop |
| E6 | best + `perfmap=1` (same branch) | `STARLTE PERFMAP` lines: PLL_MIF rate, the APM's MIF / INT / G3D tables | MIF below 1794 MHz: open the MIF task; a G3D level above 572 MHz at 950 mV or less: the owner decides on porting `mali_max` |
| E7 | best + `gov_policy=3` (owner's yes first, §4 item 8; held at 1794 MHz since 17 November, `OCP.md` (3a), so this is parking alone) | `stat` big ms above 1794 (0 now), temperatures, gaps, any reset | keep only if gaps improve, big under 83 C, no reset |
| E8 | the rebuilt bundle with `STARBITE_DRM_SEQ=1` in `mtl2-env` | the shim's `SBDRM ... fences by chain number on` line, `drm[pid] fences 10 s:`, `STARLTE GPU WAITS`, frames, gaps, no GPU fault | the expected step from 25 toward 38 a second on s3a's content |
| E9 | best + `PANVK_DEBUG=noafbc` in `mtl2-env` | per-chain time | what AFBC buys (a measurement) |

## 6. Not done here

- Zero-copy (§4 item 5, stage B): designed in [PRESENT.md](PRESENT.md), not built; the present by the DPP (stage A) is built, not run.
- A change of the memory bus's rate (§4 item 6).
- The Metal bundle with #327 (no PanVK build tree on the Mac mini).
- Reading SpringBoard's threads with the profiler (`sweetspeed&16384` and `tools/ios27/perf/prof.py`): §2.3's 16 ms of
  SpringBoard CPU a frame is a sum over its threads; whether its main thread is the limit after the fences is N.
- The built-in default `sweetspeed=100351` (`osfmk/arm/arm_init.c`) lacks the GPU queue (262144) and the other Liquid Glass bits that phone 1's line has. A boot whose loader line names no `sweetspeed` runs the Metal compositor without the queue (`LIQUID-GLASS.md`: 29 % of frames on time against 67 %). Whether to change that default is the owner's.
