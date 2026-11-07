# Screen-off power on the Galaxy S9

Status: **built, not yet run on a phone** (7 November). Everything here is behind boot-args that are off by default.

## What happens today when the screen goes off

- **No system sleep.** `IOPMrootDomain` refuses it (`gSleepDisabledFlag = 1`; `syssleep=1` gives Apple's behaviour back):
  with nothing to sleep into and nothing to wake from, powerd's sleep request ended in `IOCPUSleepKernel()` and never
  came back. The phone stays awake with the screen off; the side button or a tap wakes the screen.
- **The cores idle in WFI** (`cpu_idle()`, `osfmk/arm64/cpu.c`): a clock gate only. Nothing asks the firmware for a
  core power-down (PSCI `CPU_SUSPEND`) or a cluster power-down, so every running core keeps its supply and leaks.
- **The display is not told.** backboardd sends its power request to the display service (IOMobileFramebuffer selector
  12, one scalar: seen at boot as 1, 0, 1), and `MiniIOMobileFramebuffer.cpp` answered "done" and did nothing. The
  screen goes dark by its brightness alone: the S6E3HA8 keeps scanning in display-on, the DSIM and DECON stay up.
- **The front camera** (`cam_s9=7`) no longer streams from boot: since `cc2b18f9` it starts at the first read of
  `/dev/hakios-camera` and stops after the last close or `STREAM_IDLE_MS` without a read (`cam_s9_always=1`: the old
  behaviour). Its power cost with and without a reader is still to be measured.
- **Modem**: parked (not started), nothing to do.

## What this adds (patch 1517, `drivers/display/panel/hakios.s6e3ha8.c`)

| Boot-arg | Default | What it does |
|---|---|---|
| (none) | | every change of the display power request is logged: `GenericARMPE: the display off (iOS's power request)` |
| `panel_sleep=1` | off | with `panel=1`: screen off sends display-off (28) and sleep-in (10) between the level-1 key, as Samsung's display-off and exit sequences do; screen on sends sleep-out (11), the last brightness again, display-on (29). The supplies stay on, so the panel keeps what the boot loader set it to (DSC, PPS, TE): whether it all survives a sleep-in is the first thing the phone run shows |
| `sleep_cores=1` | off | `sleep_cores_after=` seconds (10) after the screen went off, the performance cores are taken off with `processor_exit()` (each ends in PSCI `CPU_OFF`; with all four off the firmware may power the cluster down); at screen-on they are started again with `processor_start()` (PSCI `CPU_ON`, the boot's path). `sleep_cores=2`: the efficiency cores too. Never the boot processor, never the loader's core (`core0=1`) |

Kernel pieces of `sleep_cores=` (all in patch 1517): `ml_cpu_can_exit()` (`osfmk/arm/cpu_common.c`) allows exactly the
cores whose bit GenericARMPE has set in `hakios_cpu_exit_mask` (without `sleep_cores=` it stays 0 and `processor_exit()`
is refused as before); `cpu_sleep()` (`osfmk/arm64/cpu.c`) sets the sleep token that `cpu_exit_wait()` waits for, which
only Apple's path set; `psci_smp_start_cpu()` waits for `AFFINITY_INFO` to say "off" before a second `CPU_ON` of a core.

## How to measure (read-only)

`max77705=1 max77705_fg_s=30` logs the gauge every 30 s: RepSOC, RepCap, Current, AvgCurrent and the raw coulomb counter
QH; `max77705_diag=1` adds the charger's own registers (0xB2-0xC5) to every 5-minute line. Nothing is written to the
MAX77705. With the phone on USB the charger feeds the system, so the battery's current shows the load only while the
input is the limit (or with the cable out): compare runs under the same supply, screen off 30 minutes each:

1. baseline: today's image, screen off by the side button;
2. `panel_sleep=1`;
3. `panel_sleep=1 sleep_cores=1`;
4. `cam_s9_always=1` against the default, for the camera's share.

What to look for in the log besides the numbers: `s6e3ha8: panel_sleep:` lines (packets sent), the picture after the
screen comes back, `GenericARMPE: sleep_cores: cpu N taken off` / `started again` with their times, and
`psci: cpu N: started again; AFFINITY_INFO`.

## Not done (next steps)

- Core power-down in idle (PSCI `CPU_SUSPEND` from `cpu_idle()`, waking through the reset vector as Apple's deep idle
  does) and the SoC's own idle states through ACPM: bigger, and riskier than taking cores off while nothing runs.
- The DECON and DSIM clocks while the panel sleeps; the touch controller's scan rate (tap-to-wake costs a scanning
  controller); the frame thread's behaviour with a sleeping panel (no TE).
