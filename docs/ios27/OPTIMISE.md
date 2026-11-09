# iOS 27 on the Galaxy S9: where the time goes, and what was cut (9 November 2026)

What one day of measuring on one S9 found out about iOS 27's overhead after the speed and night runs (`SPEED.md`, `NIGHT.md`): a sampling
profiler in the kernel, the cost table it gave, and the changes made against the biggest entries: FP16 forms the Mongoose M3s can run without a
thread pin, a late-MIDR SIGILL race, the scheduler's core recommendation, Panfrost's batch splitting, the DRM stand-in's user-space answers, and the
surface hash of the GL glue. Kernel changes are behind a bit of `sweetspeed=<mask>` (default 0), except the SIGILL fix, which only runs on a path that
ended in a SIGILL before. The root edits are tools you run on your own root image; nothing of the builder's images, caches, roots, logs or the shared
cache's symbols is here.

**What is verified, and how.** The numbers come from a phone with kernels that had these changes behind `sbx` bits (one bit each: the command line is at its
607-byte limit) and roots edited as described; the table of boots is the work's own log, not a repeat. This repository carries the kernel part as
`xnu/files-12377/osfmk/arm64/sweet_prof.c` (new), edits to `sweet_a64e.c`, `starlte_smp.c`, `hakios.exynos_cpufreq.c` and patch
`patches/xnu-12377/starlte/0429` (the hooks in Apple's `sleh.c`, `processor.c`, `sched_prim.c`, `timer_call.h`, `files.arm64`, and the port's extra-RAM,
`IOKitBSDInit.cpp` and `SweetFramebuffer.cpp` code), with the bits renumbered into `sweetspeed`. The applied tree was compared with the tree that ran: they differ by
the switches, comment wording, and two additions made while porting (the profiler's emulator timing and the framebuffer's gap counters only run with the profiler's
bit on; the retry of a late-MIDR trap stops after four tries at one pc). **The gated kernel was built (the iOS-target STARLTE kernel, as for #255, #260 and #261) and
not booted.** The Mesa and `eagl.c` changes are the ones that ran (the Mesa patches apply to Mesa 25.0.7 plus this repository's patches and give the sources that were
built, up to comments; the user-space libraries were not rebuilt for this PR), with off switches added in the porting (`STARBITE_DRM_STATS`, `SBGL_FULLHASH`). "Should"
below is a guess.

## The bits

`xnu/files-12377/osfmk/arm64/sweet_a64e.c` has the table; 1 to 16 are in `SPEED.md`, 32 to 8192 in `NIGHT.md`.

| Bit | On the phone it was | Change | Where |
|---|---|---|---|
| 2 (existing) | always on | now also `FCCMP`/`FCCMPE`/`FCSEL` H, `FMOV`/`FABS`/`FNEG` H (scalar) and `FABS`/`FNEG` `.4H`/`.8H` are emulated on a Mongoose M3 instead of pinning the thread to an A55 | `sweet_a64e.c`, `emulate_fp16_m3_more()` |
| (none) | always on | a trap taken on an M3 but handled after the thread migrated to an A55 (`sleh_synchronous` enables interrupts, so it can be preempted and moved before the MIDR is read) is run again instead of ending in SIGILL; at most four tries in a row at one pc, so an unallocated encoding inside one of the patterns still ends in the SIGILL | `sleh.c`, `sweet_a55_retry_ok()` |
| 16384 | `sbx` 512 | SWEET PROF: a 1 ms (`sweetprof_us=`, at least 100) per-CPU sampling profiler, armed only while a CPU runs a non-idle thread; every 10 s `SWEET PROF (10 s): N samples ... pid u/k/m3:`, `SWEET PROF pcs: u\|U\|k\|K<pid>:<pc>:<count>@<cpu>` (capital: on an M3), `SWEET PROF map:` for pcs outside the shared region, and `SWEET PROF emu:` (time inside the LSE and arm64e emulators, retries); and the framebuffer's `A5XELTE FB gaps: 1:.. 2:.. 3:.. 4-6:.. 7+:..` (vsync ticks between presented frames) | `sweet_prof.c`, `starlte_smp.c`, `SweetFramebuffer.cpp` |
| 32768 | `sbx` 1024 | the extra RAM canary pages, as `extra_ram_canary=1` (one 4 KB page in 4 MB of each extra range kept out of the VM, filled with its own address and checked every 10 s; a changed page says that something outside the kernel writes there) | `arm_vm_init.c`, `machine_routines.c` |
| 65536 | `sbx` 2048 | recommended cores 0xf1 (cpu0 and the four M3s) once the machine is up: the scheduler here is symmetric, so half of the UI's work else lands on A55s | `starlte_smp.c` |
| 131072 | `sbx` 4096 | the A55s' governor floor 1456 MHz (measured: no change; kept for the record) | `hakios.exynos_cpufreq.c` |

`sweetm3unpin` stays a boot-arg (`EIGHT-CORES.md`); the owner's build ran it at 2 ms.

## What the profile found

Boot-time and trap-cost work had left a UI at 35-40 fps in the scripted bench (a 300 ms drag every 2 s; 16 home-screen swipes, then Settings, then 16
scroll swipes: `bench.py`). The profiler (`tools/ios27/perf/prof.py`, `costtab.py`, with a symbol index of your own shared cache: `dscsym.py`) says:

- **`backboardd` is CPU-bound, not GPU-bound.** The GL glue's own statistics over 1400 frames of one boot: 8.9 ms of CPU per frame, of which 0.46 ms waiting for the GPU and 0.06 ms
  flushing; heavy frames 30-40 ms (813 GL calls, 179 draws). A fit over 4858 logged frames: CPU µs = 169 + 57 per GL call (about 320 µs a draw). The GPU
  was busy 50% of the time with 1229 job chains a second before the changes, 36% after.
- **`libOSMesa` itself does not trap**: built arm64 (v8.0), `-O3` release `NDEBUG`, no LSE, no FP16 arithmetic. The traps are the system's own code.
- **The cache's FP16** (`fp16census.py`): CoreGraphics 729 `FCMP` H and 1007 FP16 vector instructions, QuartzCore 65 `FCMP` H, 101 `FMOV W,H`, 43 FP16 vector, libswiftCore about 160 scalar. A
  thread that runs a vector FP16 instruction is pinned to an A55 (it cannot be emulated at the speed the code runs it); a `FCCMP`/`FMOV H` or `FABS H` is what the first boots'
  pins were from, once the `FMOV`/`FCMP` of `SPEED.md` were emulated.
- **The governor**: the M3s ran at 1794 MHz (the stock 4-core maximum, `cpufreq_gov_big_min`); the A55s from 455 MHz, with the frame boost for the M3s only, so UI threads that land
  on an A55 may run at 455-1000 MHz. A 1456 MHz floor did not change the frame rate (o13).
- **The scheduler** (the build has no `__AMP__`) put about half of the UI work on the A55s; recommending only cpu0 and the M3s helps, but the render thread was still bound to an A55 by FP16 pins until the emulation grew. The share of `backboardd`'s samples on an M3 in the boots below:
  14% (o1), 33% (o5, the soft pins released after 2 ms), 73% (o8, the FP16 forms emulated), 85% (o12), 90% (o13).
- **IOKit calls**: 31 thousand "other" calls to the GPU user client every 10 s with the stock stand-in for libdrm; 1.7 thousand once `MADVISE` is answered in user space (StarbiteMali never purges a
  buffer; its `MADVISE` only checks the handle and says "retained", and Panfrost's BO cache asks twice per cached buffer reuse), and about 15 with `MMAP_BO` answered from a per-handle cache.
- **Batches**: about 16 submitted batches a frame; why (`STARBITE_DRM_STATS=1`): dependency on readers about 8, on a writer about 7, flush-all about 1. CoreAnimation reuses its offscreen
  textures many times a frame, and each full clear of one used to submit the passes that sampled it. Giving the target a fresh BO (as `transfer_map` does for a discarding write) cut the dependency submits
  from 3200 to 1500 every 10 s and the batches by a quarter (about 2000 renames every 10 s).
- **The surface hash**: `libsbgl`'s `surface_changed` read each IOSurface in full every frame, about 7% of `backboardd`'s time (uncached memory). Every 32nd word in between, in full every 32nd frame:
  a change only the full sum sees shows at most 31 frames late.
- **Extra RAM**: the loader's seven ranges (`adt-extra-ram.py`) of 238, 472, 104, 70, 191.5, 128 and 201 MB: mask 3 (710 MB) was stable; all seven (mask 127, 1404 MB) gave one SpringBoard SIGSEGV at about 70 s
  on a boot of 8 November, and none on the boots below, with the canary on (0 changed pages).

## The boots

Scripted bench; "fps" is the framebuffer's 5-second windows (a drag mixed with static time: it saturates near 40 and hides per-frame gains; the gap table below is the better measure).

| Boot | Change against the previous | Swipes fps median (p10/p90) | Scroll | `backboardd` CPU / on M3 | Notes |
|---|---|---|---|---|---|
| o1 | the night's final kernel + the profiler + the bench | 35.6 (27.7/37.9) | 18.1 | 50% / 14% | GPU 1229 chains/s 50%; 20 ms in the emulators per 10 s |
| o2 | cores 0xf1 | 36.5 | 18.3 | 50% / 19% | the render thread is bound to an A55 (pins) |
| o5 | `sweetm3unpin=2` (was 100) | 39.3 | 18.2 | 43% / 33% | `iconservicesagent` SIGILL x3: the late-MIDR race |
| o6 | the retry of a late-MIDR trap | 38.7 | 18.4 | 43% / 31% | `backboardd` samples on cpu2: pinned by `FCCMP` H |
| o7 | root: MADVISE in user space, sparse surface hash | 38.6 (37.5/42.5) | 20.4 | 48% / 32% | IOKit "other" calls 31k to 1.7k per 10 s |
| o8 | kernel: `FCCMP`/`FCCMPE`/`FCSEL`/`FMOV`/`FABS`/`FNEG` H, `FABS`/`FNEG` vector | 39.3 (26.6/43.0) | 19.6 | 30% / 73% | no `backboardd` or SpringBoard pins left |
| o9 | `extra_ram_mask=127` and the canary | 40.0 (29.7/41.3) | 19.4 | 30% | all 1404 MB, 0 canary pages changed |
| o10 | root: `MMAP_BO` answers cached | 38.0 (31.9/39.9) | 20.4 | 31% / 65% | "other" calls about 15 per 10 s |
| o11 | o9 with all eight cores recommended | 39.9 | 19.6 | 40% / 51% | same fps, 10 points more CPU: keep 0xf1 |
| o12 | root: render-target renaming | 37.8 (32.1/40.2) | 20.1 | 29% / 85% | batches -25%, GPU busy 36% |
| o13 | A55 floor 1456 MHz | 40.0 | 19.1 | 28% / 90% | no change: dropped |
| o14 | the present-gap histogram | 40.6 | 18.5 | 27% | |

The gaps (ticks between presented frames; "on time" = one tick, "motion fps" = 60 * frames / ticks over gaps 1-3), `gaps.py`:

| Boot | Swipes on time / motion fps | Scroll on time / motion fps |
|---|---|---|
| baseline: the night's final config (mask 3, unpin 100, no 0xf1) and the original libraries | 69% / 45.1 | 55% / 38.5 |
| the final: the config above, final libraries | 74% / 45.9 | 69% / 42.8 |

What is behind it: the render thread no longer waits on A55 pins, `backboardd` takes 27-33% of a CPU where it took 50%, most of its time is on M3s, and the GPU works 36% of the time where it worked 50%. The
frame rate moved less than the CPU: the scroll gain (55% to 69% of frames on time, 38.5 to 42.8 fps in motion) is the clearest, and the home-screen swipes gained about 5 points. The last boot (the owner's, the final
kernel's configuration with the profiler and the bench off) showed 1404 MB extra RAM, canary round 0 clean, cores 0xf1, frames presented, idle CPU about 3%.

## What the changes are not

- Not a repeat on this repository's gated kernel: the bits are renumbered, nothing here has booted.
- `sweetspeed&32768` is the canary, whose cost is one page in 1024 of the extra RAM; the pages are never the VM's.
- The Mesa patches change what Panfrost does on a full clear (`SBGL_NORENAME=1` in `backboardd`'s environment turns the renaming off; `STARBITE_DRM_KMADVISE=1` asks the kernel
  `MADVISE` again) and the glue's surface hash (`SBGL_FULLHASH=1`: in full every frame, as before). The glue's `glenv=NAME=VALUE` (`eagl.c`) puts them into the environment, at the price of command-line room.

## What is left

- **CPU-side render cost is still the wall**: 320 µs a draw in Mesa and CoreAnimation; most of `backboardd`'s remaining time is in `libOSMesa` (Panfrost's state emission) and the glue's copy of the frame,
  then CoreAnimation's own render; `costtab.py` splits it. The ideas that stayed ideas: zero-copy scan-out to the DPU, fewer GL calls per draw (a state cache above Mesa), Metal in
  CoreAnimation (tried in `NIGHT.md`: slower).
- **A55 pins** still happen for FP16 vector arithmetic (CoreGraphics' 1007 sites); they are the right call at the speeds those loops run, and `sweet_a55_retry_ok()` makes their migration race harmless.
- **The scheduler** is symmetric; a real AMP table (perf levels for the M3s) would let the A55s take background work without the 0xf1 recommendation.
- **Command-line room**: `glenv=` for the Mesa switches, and a `sweetspeed` that fits all of them (`starlte-owner.txt` is at 593 bytes of 607).

## Tools

`tools/ios27/perf/`: `prof.py` (aggregate the profiler's lines by process, image and function), `costtab.py` (where one process's samples go), `gaps.py` (the gap table), `summ.py`
(a boot's summary), `fp16census.py` and `fp16sites.py` (what a cache's images contain that traps), `dscsym.py` (the symbol index of your own cache; the index is derived from it and stays on
your machine), `build-sbgl.sh` (the glue with the libOSMesa of `mk-mesa-ios.sh`), `test/fp16t.c` (the FP16 emulation against the hardware: 21.6 million checks, no mismatch). `tools/ios27/speed/`: `edit-libs.sh`,
`edit-o7.sh` (the libraries into a root; the job off), `bench.py`. `tools/ios27/cmdline/starlte-owner.txt` is the command line of the owner's build: `starlte-fast.txt` with
`extra_ram_mask=127`, `sweetm3unpin=2` and `sweetspeed=98303` (2047 of the night run, 32768 the canary, 65536 the cores).
