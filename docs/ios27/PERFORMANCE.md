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

## 7. Jobs a draw: PanVK's descriptor copies (18 November; `STARBITE_PANVK_CPU_DESC=1`, built, NOT run on a phone)

**What the logs say.** On phone 1's F1 boot (18 November, home-screen swipes) the GPU queue's profile gives, every 10 s,
about 1,830 vertex/tiler chains at 1.33 ms on average and 1,080 fragment chains at 1.62 ms: about 2.4 s and 1.75 s of
GPU time, roughly 8 ms and 6 ms of a 14 ms GPU frame (M: `STARLTE GPU QUEUE prof`). The vertex/tiler side is the larger,
for draws of a few hundred vertices. metal-perf's PanVK statistics (12 November, `STARBITE_DRM_STATS=1`) count about
10.5 jobs a batch in the 26-pass, 93-draw swipe frame, about 3 jobs a draw (I: 3.6 draws a pass).

**Why three.** PanVK's JM path (Bifrost) copies a draw's descriptors (UBOs, textures, samplers) from the bound sets into
the draw's own hardware tables with a compute job, one workgroup a descriptor (`bifrost/panvk_vX_meta_desc_copy.c`,
`meta_get_copy_desc_job`), for the vertex shader and again for the fragment shader whenever the descriptor state or the
shader changed (`jm/panvk_vX_cmd_draw.c`, `prepare_draw`). CoreAnimation changes pipeline or textures on nearly every draw,
so a draw is two copy jobs and its IDVS job. Each job in a JM chain costs the job manager's start, dependency and
completion, whatever it does; how much of the 1.33 ms is that and how much is tiling is NOT measured (B2 below says).

**What `STARBITE_PANVK_CPU_DESC=1` does** (`tools/ios27/mesa-ios/mesa26-panvk-cpu-desc-copy.patch`, in `/var/local/mtl2-env`):
the copy is done on the CPU instead, when the batch is submitted, into the same tables, before the batch is made
visible to the GPU (`pan_kmod_flush_bo_map_syncs`). Submission is the latest moment the job's own copy could see a
different descriptor: an application may update an update-after-bind set until it submits (metal2vulkan's heap sets are
such), and the copy job reads the set when it runs, after the submission. A push descriptor set is taken as its upload
takes it, at the draw. Only for a primary command buffer's draw whose shader has no image descriptors (whose attribute
pairs the job also rewrites); everything else keeps the job. Without the variable PanVK does what it did.

**Counters** (with `STARBITE_DRM_STATS=1`, whatever the switch): the shim's 10 s line
`drm[pid] vk draws D; descriptor copies: jobs J (N descriptors), on the CPU C (M descriptors)`. With the switch off,
J / D is the copy jobs a draw; with it on, J should fall to the image cases and C take their place.

**The test** (scion-perf60's root steps `edit-m2.sh add|cpu|bins`, bundle StarbiteMetal 8c622664 + libvulkan_panfrost
1dcd6b14, the d5 F1 kernel): B1 `add` (counters only, the switch off) against B2 `cpu`. Read `GPU QUEUE prof` (vertex/tiler
average and count), the `vk batches ... jobs` and `vk draws` lines, fbt's CoreAnimation swap period, the swipe fps, and look at
the home screen for wrong textures (a wrong copy shows as wrong icons or a GPU fault line). Expected, if the job count is
the cost: the vertex/tiler time falls by up to two thirds, a few ms of the frame. If it does not fall, the time is in the
IDVS jobs themselves (tiling), and `STARBITE_PANVK_BINS` (B3) is the next knob.

**Result on phone 1 (18 November, d5's F1 kernel, the same 16 scripted page swipes, logs `con-xnu-boot-f1-1118-1210` B1
and `-1218` B2, 30 s of swipes each):**

| | B1 (counters only) | B2 (`STARBITE_PANVK_CPU_DESC=1`) |
|---|---|---|
| descriptor-copy compute jobs / 30 s | 61,198 | 1,116 (images only); 62,490 copies done on the CPU |
| PanVK jobs a batch | 47.0 | 34.1 |
| vertex/tiler chains, average | 1,343 us | 1,311 us |
| vertex/tiler GPU time / 10 s | 2,067 ms | 2,091 ms |
| swipes (tester's bench) | 39 % / 36.6 fps | 39 % / 36.7 fps |
| CoreAnimation swap period p50 (fbt) | 33.5 ms | 33.6 ms |

No wrong texture, no GPU fault. **The copy jobs were not the cost**: a quarter of the jobs went and the vertex/tiler time
did not move, so they ran beside the draws' critical path. The switch stays off by default.

The same boots also measured the encode split for the first time (the pass log's bridge column): in the home screen's
main pass, 79 % of an encoder's time is this side's (the frontend, the runtime and PanVK's recording), **82 us a draw**,
7.1 ms of a 9.0 ms main pass of 87 draws; small passes pay 150 to 300 us each for one or two draws.

## 8. Indexed draws on the GPU's indirect path (18 November; `STARBITE_PANVK_CPU_MINMAX=1`, built, NOT run on a phone)

**What PanVK does with `vkCmdDrawIndexed` on JM** (`jm/panvk_vX_cmd_draw.c`): it is recorded as an indirect draw. Before the
draw's IDVS job come a write-value job (the search's start values), a compute job that searches the draw's indices for their
range (`panlib_draw_index_minmax_search_helper`) and a helper compute job that patches the draw's jobs and descriptors from that
range (`panlib_draw_indexed_indirect_helper`, with the job manager's prefetch suppressed), each waiting for the one before.
Most of CoreAnimation's draws are indexed (B1: ~1,200 direct draws against ~61,000 descriptor copies in 30 s). A one-draw
pass costs about 77 us of vertex/tiler chain and the 87-draw main pass about 8.7 ms (sgu_dump, 17 November): about 80 to
100 us a draw whatever the draw's size, which is what a chain of four dependent jobs a draw would cost, and what B2 did not
touch (its jobs were not in that chain).

**What `STARBITE_PANVK_CPU_MINMAX=1` does** (`tools/ios27/mesa-ios/mesa26-panvk-cpu-minmax.patch`): for a direct indexed draw
whose index buffer's memory is mapped (Metal's shared buffers are), the CPU reads the draw's indices when the command is
recorded and writes their range where the search job would; the write-value and search jobs are not made, and the helper
depends on what they depended on. The range is the search's (restart indices skipped; none valid gives the same empty
range). A buffer that is not mapped, or a range past the bound size, keeps the GPU's way. The indices must be those the GPU
will read: an index buffer the GPU writes earlier in the same submission would be read stale; metal2vulkan's only such
writer (the restarted-strip rewrite) writes device memory, which is never mapped. Counters on the shim's line: indexed/indirect
draws, and the ranges found on the CPU and not.

**Expected** if the dependent-job chain is the cost: the chain per indexed draw goes from four jobs to two, the vertex/tiler
time towards half (about 3 to 4 ms of the 14 ms GPU frame), and PanVK's recording of each draw lighter. Removing the helper
too (patching the draw's jobs on the CPU, as the CL helper does) would leave one job a draw; it is not built.

**Result on phone 1 (18 November, log `con-xnu-boot-f1-1118-1236`, the same kernel and swipes as B1):** all 30,921
indexed draws of the 30 s had their range found on the CPU; the write-value and search jobs went (jobs a batch 47.0 ->
33.7). The vertex/tiler chain average went from 1,343 to 1,277 us (-5 %), its GPU time from 2,067 to 2,002 ms per 10 s
(-3 %); swipes 36.6 fps, swap period p50 33.5 ms, as B1. The home screen rendered correctly, no fault. **So the dependent
chain before the draw is not the cost either.** What is left a draw is the helper job and the IDVS job: about 190 us of
vertex/tiler time a draw (2.0 s per 10 s for about 10,300 draws), which neither of the two removals touched. Whether that
is the helper's patching, the job manager's prefetch suppression around it, or the tiling itself is not known; the next
experiments are `STARBITE_PANVK_BINS` (fewer bin levels, no build) and a debug build that submits the helper and the IDVS
job as separate chains so the GPU queue's profile times each. The switch stays off by default.

## 9. What holds a home-screen swipe at 30 a second (18 November; the timeline from the F1 boots' traces)

**The question.** A busy swipe frame costs about 9 ms of CPU encode and 13 to 14 ms of GPU (sections 7 and 8). Each half
fits in 16.7 ms on its own. One reading was that they run in series: frame N+1's encode would not start until frame N's GPU
work is done. If that were true, a SwapEnd that returned sooner (and a third surface) would overlap them and give ~60.

**What the code does** (the fork's `iokit/Kernel/arm/SweetFramebuffer.cpp` at `scion/xnu-12377` 27a3e12):
- `AppleCLCD::swap()` (SwapEnd, line 1228) waits for the chains of the swap before it (line 1252,
  `starlte_gpu_user_wait_seq(prev, true)`), then records the frame. So at most two frames are in flight, and SwapEnd(N)
  returns as soon as frame N-1's chains are done, not frame N's. The encode of N+1 already overlaps the GPU work of N.
- The vsync tick (`vsyncFired`, line 735) sends the vsync notification first (line 755), takes the newest recorded frame,
  sleeps until that frame's own chains are done (line 875) and presents it (line 891; 1.9 ms, a GPU job under the GPU's lock).
  While it sleeps the next tick is late.
- backboardd's render thread renders from its display timer. Its burst sample (PRESENT.md section 7) puts 59 % of its time in
  that wait, 15 % in SwapEnd and 26 % running. No sample of it is in a drawable or IOSurface wait. The thread
  `metal2vulkan-queue-wait` that sits in a wait 40 % of the time is the bridge's completion thread, not the render thread.

**The traces.** Every F1 boot ran with `sweetfb_trace=70`: a ring of the last 768 framebuffer events (about 3.5 s, during the
scripted home swipes) with their times. `tools/ios27/perf/fbt-regimes.py` splits a ring into runs where the app GPU queue
never ran empty (from `B` to `E`) and the swaps outside them. In every boot the swaps fall into two clean classes:

| boot (log `con-xnu-boot-…`) | GPU-busy runs: ms, chains, swaps, **queue ms a swap** | busy swaps: period p50, SwapEnd hold p50, tick→call p50 | idle swaps: n, period p50, hold p50, tick→call p50, chains |
|---|---|---|---|
| f1-1118-1210 (B1) | 545, 275, 16: **34.1**; 105, 38, 4: 26.2 | 35.4, 18.8, 11.6 ms (n 28) | 61; 33.5, 0.0, 5.5 ms; 6 |
| f1-1118-1218 (B2) | 521, 220, 16: **32.6**; 85, 49, 3: 28.5 | 34.8, 13.9, 11.5 (n 24) | 63; 33.4, 0.0, 5.5; 6 |
| f1-1118-1236 (M3) | 318, 128, 10: **31.8**; 114, 47, 4: 28.6 | 35.3, 15.9, 10.4 (n 22) | 64; 33.5, 0.0, 5.6; 6 |
| sp2-1117-2231 | 435, 187, 13: **33.4**; 275, 168, 9: 30.5 | 35.5, 17.2, 10.7 (n 29) | 61; 33.3, 0.0, 4.0; 6 |

"Queue ms a swap" is the time the app queue was not empty divided by the swaps in the run. It includes the present (about
1.9 ms a frame), which holds the GPU while a queued chain waits.

**One busy frame** (f1-1118-1210's ring; ms from the first event; letters as in the trace's comment, SweetFramebuffer.cpp line 80):

    2.13  T  tick, 19 chains queued;  F 362: it takes swap 362 and sleeps for its chains
   16.12  S  CA calls SwapEnd for 363, 14.0 ms after the tick (its render); 32 chains queued
   35.30  G  362's chains done: the tick wakes;  R 363: SwapEnd returns (held 19.2 ms, for 362)
   37.25  P  362 presented
   39.63  T  the next tick, 37.5 ms after the last (it slept);  F 363
   53.40  S  364 (13.8 ms after its tick), 32 queued
   72.16  G  363 done;  72.17 R 364 (held 18.8 ms);  74.06 P;  76.49 T  F 364 ...
  306.79  E  the queue runs empty for the first time: 480 chains since it last started

**One idle frame** (the same ring):

  1003.57  T  tick, nothing queued (no new swap: the render thread waits for its timer)
  1009.61  B  the first chain of swap 387;  1009.99 S 387 (6 chains), R 387 at once (held 0)
  1017.89  E  its 6 chains done (7.9 ms of GPU)
  1020.01  T  F 387, G at once;  1021.83 P
  1036.90  T  nothing new again;  1041.40 B  1041.76 S 388 ...

**What that says.**
1. **Busy swaps** (the page moving, 0.3 to 0.55 s a swipe) are **GPU-bound**. The queue does not run empty for half a
   second, and each swap costs 32 to 34 ms of GPU queue time. The SwapEnd hold of 14 to 19 ms is CA waiting for a GPU that
   has no idle time. The next encode already overlaps: the swap call comes 10 to 12 ms after the tick, while the GPU is still
   on the frames before it. A SwapEnd that returned at once, with a third surface, would give CA's frames nowhere to run: the
   GPU would queue them up, adding latency, not frames. **The CPU/GPU serialisation hypothesis is false.** The overlap change
   is not built.
2. **Idle swaps** (two thirds of the swaps in every ring) are small frames: 6 chains, 4 to 8 ms of GPU, 4 to 6 ms of render
   after the tick, nothing held. They come exactly every second tick (p50 33.3 to 33.5 ms) while the GPU and the render thread
   are idle in between. This cadence is set **upstream of the GPU stack**: the window server has nothing new at the tick in
   between. These swaps come in runs of about a second after each swipe's busy run. Which producer they come from is
   **N**: SpringBoard's commits, or an animation that asks for 30 Hz. SpringBoard's main thread was running in 26 % of its
   samples over the 27.6 s window (`smpburst=SpringBoard`, the same boots: 1,426 of 5,514 at 5 ms), so it is not saturated
   on average. The sampler does not print the stacks of running samples, so where those 26 % go is not known.
3. **The yardstick hid the GPU.** The CA swap period's p50 of 33.5 ms that sections 7 and 8 compared is the median over both
   classes. The idle swaps dominate it, and their cadence does not depend on the GPU. A GPU change shows only in the busy
   class: the GPU queue time a swap of its runs and the busy-swap period. `fbt-regimes.py` prints both. (Sections 7 and 8
   are still negative on their own measure: the vertex/tiler GPU time did not move.)
4. **Why `sweetfb_nowait` v2 was slower** (xnu PR 47, boot nw2, 17 November: 30.5 against ~38 fps). It removed the tick's
   sleep (line 875), not the SwapEnd hold. With the GPU saturated, a frame not done at its tick stays recorded, and the next
   swap replaces it. Its log's busy windows: "frames shown done" 73 to 90 per 5 s, "frames a newer one replaced meanwhile" 43
   to 86. A third to a half of the GPU's frames were drawn and never shown, on a GPU with no time to spare. Overlapping more
   cannot help a saturated GPU; dropping finished work makes it worse.

**So, for 60 during the page movement:** the busy frame's GPU time must halve, from about 32 ms to under 16.7 ms. The 10 s
profiles put about 58 % of it in vertex/tiler chains and 42 % in fragment chains (f1-1118-1210 12:12:08: 1,819 × 1,335 us
against 1,075 × 1,619 us), so about 18 ms and 13 ms a busy frame, plus the present (I: the window's split applied to its busy frames). For the frames after a swipe, the 30 Hz
producer must be found first. No GPU or framebuffer change moves it.

### 9.1 Indexed draws as direct draws (`STARBITE_PANVK_CPU_DIRECT=1`, built, NOT run on a phone)

Sections 7 and 8 removed the jobs around a draw that ran off the critical path, and the time did not move. What is left of
an indexed draw is its helper compute job and its IDVS job. The helper (`libpan/draw_helper.cl`,
`panlib_draw_indexed_indirect_helper`) is one invocation that patches the draw's job, varyings and attributes from the
index range. It runs with `PANLIB_BARRIER_JM_SUPPRESS_PREFETCH`, and the IDVS job depends on it. So every draw of a pass waits
for a one-thread compute job to be dispatched, to run and to write back before the job manager fetches the draw. That is a
pipeline drain a draw, whatever the draw's size, and it fits the ~100 us a draw of the 87-draw main pass.

`mesa26-panvk-cpu-direct.patch` (after `mesa26-panvk-cpu-minmax`): with `STARBITE_PANVK_CPU_DIRECT=1` in `mtl2-env`, an indexed
draw whose index range the CPU found (the switch turns section 8's range search on too) is recorded on the direct path,
as Mesa 24's PanVK recorded every indexed draw. The IDVS (or vertex + tiler) job gets its index pointer, index count,
`offset_start` (= min + vertexOffset), `base_vertex_offset` (= -min), invocation count (the range), instance size and
varyings from the command buffer's pool when the draw is recorded. These are the values the helper would write. No helper,
no prefetch suppression and no dependency come before the job. A range with no valid index keeps the indirect path.
Without the variable PanVK is unchanged. The shim's line shows it: `indexed/indirect` falls to about 0 and `vk draws`
takes their place.

Built: the patch series reproduces the built tree from Mesa 26.2.6's tarball. Bundle m4: StarbiteMetal ad08e6dc… (its
`__text` identical to m3's 7c953578) and libvulkan_panfrost 22bd0c44…. Risk: a wrong range or offset is a wrong or missing
icon, not a fault. The range is section 8's, which rendered correctly on the phone.

**On phone 1 (18 November).** The phone had just moved to iOS 27.2.1 (24B156). The F1 image does not boot it, so all three
boots used the tester's `b272` image (2390a6aa: the 24B156 owner image, kernel m1118g 97991e68, with the scripted bench,
`sgu_prof=1 sweetfb_trace=70 sweetfb_prof=1 smpburst=SpringBoard`). They differ only in `edit-m4.sh`'s mode. Logs
`con-xnu-boot-b272-1118-…`:

| boot | mode | result |
|---|---|---|
| P0 `-1354` | `base` (switches off) | renders. Tester's bench: swipes 89 % / 52.4 fps, scroll 21 % / 23.9 (on 24B156 the scripted in-app drag opens News), 81 C, background churn. The trace ring fell in the News drag: swap p50 70.0 ms |
| P1 `-1403` | `bins` (`STARBITE_PANVK_BINS=aa`) | **stuck after the third frame** (Apple logo, no fault line, no frames in the bench windows). As set, the switch is unusable; it measured nothing about tiling |
| P2 `-1410` | `direct` | **renders correctly**: all icons, widgets, glass dock (screenshot). One `async chain … FAILED … js_status=0x58` line during the swipes; the same line (JS1, gpu=0x600, 0x58) is in six older boots without this change (e0-old, s3a, nw2, cand3sb, bt2e, bt2), so it is not this change's. Bench: swipes 42 % / 37.4 fps, scroll 95 % / 57.0; swap p50 33.1 ms |

The bench numbers of P0 and P2 do not compare: the script's phases fell differently against its windows (P0's
"scroll" ran in News at 81 C). The GPU queue's 10 s profile does compare, window by window, where the content is the same:

| window (same content in both) | P0 `base` | P2 `direct` |
|---|---|---|
| first swipe window: fragment chains, avg | 1,086 × 4,023 us | 1,065 × 3,819 us |
| same window: **vertex/tiler chains, avg** | 3,547 × **932 us** (816 chains ≥ 1.6 ms) | 3,400 × **416 us** (51 chains ≥ 1.6 ms) |
| second swipe window: vertex/tiler | 2,568 × 948 us | 2,185 × 403 us |
| the idle screen (400 chains / 10 s, identical) | 951 us a chain | 565 us a chain |
| all windows with > 500 v/t chains: v/t GPU ms per 10 s | 1,495 | 891 |
| PanVK jobs a batch | 20.0 | 15.4 |

**The helper was the vertex/tiler cost.** On the same frames the vertex/tiler chains take 55 % less time and the GPU 30 to
37 % less in the swipe windows. That is the first change of this series that moves the GPU. The trace's busy swaps in P2
were held 0 ms at the median, against 14 to 19 ms on every earlier boot: the GPU was no longer saturated in that ring.
What the swipe fps gains is NOT measured yet. The repeat pair (`base`, then `direct`, back to back) is requested from the
tester. Until then `STARBITE_PANVK_CPU_DIRECT` stays off by default.

## 10. Re-pinning the bridge to upstream metal2vulkan (18 November; `M2V_REPIN=1`, opt-in, built)

**How far behind.** The iOS 27 bridge (`tools/ios27/metal2/build-m2v-bridge.sh`) pins metal2vulkan at `720ef1ce`
(14 October 2026). It carries our 20 patches (`bridge/patches/0001`–`0016`, `0100`–`0103`). UniXNU qualified `46e7cc96`
(`development`, 17 November) with AdrenoMetal's consumer patches `0063` and `0064` (UniXNU's
`scripts/build-adreno-metal-bridge.py` applies exactly those two). `46e7cc96` is **6,158 commits** ahead of `720ef1ce`, and
`720ef1ce` is its ancestor. Since mid-October upstream's runtime has gained what our 0010–0014 did by hand and more:
- kept and carried draws (a queue holds the carries of its latest commits);
- a replayed draw walks its plan's heap members and resident uses once, not at each draw;
- one state lock per encoder open and one per end;
- render encoders over one target continue one render-pass instance, and record into the VkCommandBuffer of the encoders
  before them;
- a rebind rework;
- the copy engine for buffer copies;
- AdrenoMetal 0063: uniform descriptor capacity, queue fairness, copy barriers.

Several of these cut CPU per draw. "Continue one render-pass instance" may also cut GPU chains, but that is not measured.

**What is built** (`M2V_REPIN=1 build-m2v-bridge.sh`, its own source and output directories, default unchanged):
- metal2vulkan `46e7cc96` + AdrenoMetal `0063`, `0064` (unchanged, from AdrenoMetal `cc0a4e8`) + `0105` (ours:
  0064's `read_pages` on iOS too; without it the runtime does not build for `aarch64-apple-ios`). The list and the
  licences are in `bridge/patches-46e7cc96/README.md`, NOTICE and THIRD_PARTY_LICENSES.md.
- The bridge's sources (`NativeMetalBridge.rs`, `NativeMetalObjects.rs`, `NativeMetalTelemetry.rs`) build against both
  runtimes. Six places follow the new API under `#[cfg(m2v_repin)]`:
  - no `metal_level`;
  - `HostPageTexturePolicy::InPlaceWrites` (upstream's form of our 0008/0016/0101, and what AdrenoMetal sets) in place of
    `padded_reads` and `refuse_rebinds`;
  - the linear-alignment query on the device;
  - an infallible `SharedEventListener::new`;
  - no 0103 counters.
- `mk-bundle27.sh` takes `BRIDGE=<path>`. Bundle m5 is that bridge with m4's PanVK: StarbiteMetal abc1995e…, 33 MB
  against 21.6 MB.

**What is not done.** None of our 20 patches is carried. 0002 is already upstream. The other 19 conflict when
cherry-picked onto 46e7cc96 + 0063 + 0064, because upstream moved the code (`render/bound.rs`, `render/executions.rs`,
the rebinds, the linear plan). Each must be checked against 46e7cc96 and re-based only where upstream does not already do
the same. The order, by risk:
1. The correctness patches: 0006 (draws without a pipeline: backboardd died on one on 12 November), 0007/0015 (two-plane
   surfaces: the camera), 0100 (8 MiB memory blocks), 0102 (linked specialized names), 0001.
2. Start-up: 0003, 0005.
3. Per-draw CPU: 0009–0012 and 0014, only where the host bench (`tools/ios27/metal/host-bench`) shows upstream slower.
AdrenoMetal's frontend changes since 243d63cf (167 commits) are not reviewed for our frontend either.

**The phone test** (requested): bundle m5 with `STARBITE_PANVK_CPU_DIRECT=1` (`edit-m5.sh direct`) against m4 `direct` on
the same image. First see that it renders: the shader cache is cold and none of the correctness patches above is in. Then
compare the swipe windows' GPU profile and the bridge column of the pass log (µs a draw: 114 to 119 on 24B156 with the old
runtime). The re-pin becomes the default only after a phone run shows it renders correctly and is not slower.

**First boot of m5 on phone 1** (P6, 18 November 14:34Z, image b272, `edit-m5.sh direct`, log
`con-xnu-boot-b272-1118-1434`):
- It came up: the Metal device was made in 6,266 ms (cold shader cache), with no backboardd crash, no GPU fault line and no
  refused or failed MTL2 line.
- **The screen is black.** The framebuffer keeps presenting (10 frames a second at rest; the CA swap period's p50 is
  33.2 ms during the bench), but the frames are empty. The fbt numbers fit that: record to GPU done 5.0 ms, to shown 5.7 ms.
- **The pass log printed no pass at all** ("PL on" and then not one `PL w` line), where the old runtime prints dozens
  a window. So either backboardd's render passes do not reach the bridge's encoders, or the pass log's hook does not see
  them under the new runtime.
- Not found yet; the bench numbers of that boot (swipes 54 % / 40.6 fps) are of empty frames and mean nothing.
- Next step: run the host bench (`tools/ios27/metal/host-bench`, KosmicKrisp on the Mac) on the re-pinned bridge built for
  the host, which shows on the Mac whether a draw reaches its target. Then re-base 0006, 0100 and 0102 first (above).
