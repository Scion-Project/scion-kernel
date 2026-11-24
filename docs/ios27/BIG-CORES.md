# The Galaxy S9's M3s above 1794 MHz, the way Samsung's kernel does it: two of them offline (24 November 2026; run on phone 2)

Samsung's kernel lets the Exynos 9810's big cores (four Samsung M3s) run faster than 1794 MHz only with at most two of them
online, and then at 2314 MHz at most. Scion held them at 1794 MHz because it could not take an M3 offline. This page sets out
what controls the rate, the voltage and the limits on this SoC, what was built so that iOS 27's kernel can take M3s offline
and start them again (`m3off=`, off by default), the policy that uses it for the interface, and what one phone measured.

**The verdict first (measured, phone 2, 24 November).** The offline path works: 136 offline/online cycles of an M3 (22 in
the test boot, then 57 two-core cycles in two policy boots), each core read off and on again by the PMU, every one back
through PSCI CPU_ON in about 0.25 ms, no failure, no stop. With two M3s offline the other two ran at 1924, 2106 and 2314 MHz
(measured 2313-2314 MHz on the core) with the APM taking every request. **But the swipes did not get faster**: home-screen
flicks 28.4-31.0 fps with the policy against 28.5-30.8 fps without it, Settings 21.5 against 20.7-21.2.
The reason was measured before any of this ran: with the M3s pinned at 1066 MHz (41% slower) the flicks still ran at
28.5 fps and the Settings list at 21.4. So the 30 fps ceiling of the home screen is not set by the M3s' clock, and
the cost of 2314 MHz is real: +6 to +8 C at the hottest point of the run (72.6 and 74.5 against 66.3-67.2 C). The policy stays off by default.

Line numbers are those of `Scion-Project/android_kernel_samsung_exynos9810` at `lineage-24.0` (the device tree
`arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts`, "the tree"), read for facts; no line of it is in Scion.

## Contents

- (1) How the rate and the voltage are set, and the limits
- (2) Taking an M3 offline: what Samsung does, what was built
- (3) The policy and the rule above 1794 MHz
- (4) Safety: what ends a boot, and the watchdog
- (5) The phone runs
- (6) What it would take to turn it on, and what is not verified

## (1) How the rate and the voltage are set, and the limits

- **The rate.** The kernel asks the APM (the PMU's microcontroller, ACPM firmware) for a rate of a DVFS domain (2 the A55s,
  3 the M3s): domain, rate in kHz and nothing else (`cal_dfs_set_rate` -> `exynos_acpm_set_rate`). Scion does the same
  (`bsd/dev/hakios.exynos_acpm_dvfs.c` in the fork) and reads each request's answer: the APM echoes the rate it took
  (`001d5ba0` = 1924000, `00202290` = 2106000 on phone 2).
- **The voltage** is the APM's: it keeps a table of rate and voltage per level (the "fvmap", with the chip's ASV voltages in
  it) and sets the voltage with the rate. Samsung's kernel never sends a CPU voltage; neither does Scion. It reads the table
  at boot and asks only for rates in it. Phone 2's table for the M3s: 1794000 kHz 818750 uV, 1924000 868750, 2002000 887500,
  2106000 925000, 2314000 975000 (2704000 1143750 and 2886000 1225000 are in it and never asked for).
- **The limits by the number of M3s online**: `hotplug_governor` (10463-10479) `single_freq` and `dual_freq` 2314000,
  `triple_freq` and `quad_freq` 1794000; domain@1 `max-freq` 2314000, `policy-max` 1794000 (10564-10571).
  `drivers/soc/samsung/exynos-hotplug_governor.c`: the policy's maximum by the count of M3s that have not reached CPU_DEAD
  (`get_hpgov_maxfreq`, 172-186), every rate checked against the count online (`exynos_hpgov_validate_scale`, 124-145), and no
  M3 brought online while the rate is above what one more allows (`exynos_hpgov_validate_hpin`, 147-167). The cores taken
  down are `fast_hp_cpus = "5-7"` (10457-10461): cpu 4 is never.
- **Over-current**: Samsung's 9810 kernel does not build its OCP driver (OCP.md, 18 November); above 1794 MHz it relies on the
  PMIC's CPU over-current warning (S2MPS18 OCP_WARN1 bit 7, `cpu_ocp_warn_en`) and the count above. S-Boot leaves the warning
  on: phone 2 read 0x80 in every boot of this work (`ocp_guard=1` reads it; nothing here writes it).
- **Heat**: the BIG zone is cooled from 83 C (`big-control-temp`, the power allocator), its cores taken away at 96 C
  (`hotplug_out_threshold`, back at 91). Scion's governor already steps a level down a second at 83 C and to the lowest level
  at 96 C. This path adds its own stop at 75 C (below).

## (2) Taking an M3 offline

### What Samsung does

- Out, on the dying core (`exynos-powermode.c` `exynos_hotplug_out_callback`, 708-721): `cpu_disable` -> `pmucal_cpu_disable`:
  the core's CPU_INFORM register := C2 (1), then its "off" sequence (`flexpmu_cal_cpu_exynos9810.h`: the interrupt
  generator's GRP2 bid enable, a pending GRP1 bid cleared, the PMU's EVENT_INTERRUPT_ENABLE_GRP2); then PSCI CPU_OFF with the
  power-down state (`arch/arm64/kernel/psci.c` `cpu_psci_cpu_die`: 0x10000; the tree's `cpu_off = <0x84000002>`).
- CPU_DEAD, on another core (`exynos_cpu_callback`, 724-745): CLUSTER1_CPUn_STATUS (PMU + 0x2004 + 8n) polled until it reads
  off, BUG after 2 s.
- In: PSCI CPU_ON, then on the new core `exynos_hotplug_in_callback` (693-706): `cpu_enable` -> `pmucal_cpu_enable`:
  CPU_INFORM cleared, the "on" sequence.

### What was built (Scion-Project/xnu, iOS 27 fork, STARLTE)

| File | What |
|---|---|
| `osfmk/arm64/sweet_m3off.c`, `.h` (new) | `sweet_m3off_take(cpu)`: XNU's own processor offline (`processor_exit_from_user`: threads moved, timers to another core, IPIs drained, `cpu_exit_wait`), then the PMU polled until it reads the core off (2 s, else panic). On the dying core (`sweet_m3off_dying`, from `SweetCPU::quiesceCPU`): Samsung's hint and "off" writes, the sleep token, PSCI CPU_OFF 0x10000. `sweet_m3off_give(cpu)`: XNU's processor start (`cpu_start` -> `SweetCPU::startCPU`: the M3 PMU defaults and CPU_ON as at boot; XNU panics itself if the core does not arrive within `cpu_boot_timeout_secs`), the PMU read on again. On the core coming back (`SweetCPU::initCPU(false)`): CPU_INFORM cleared, the "on" writes |
| `iokit/Kernel/arm/SweetCPU.cpp` | `quiesceCPU` calls the dying path (was: panic); `initCPU(false)` and a second `startCPU` are allowed only for a core m3off took down |
| `osfmk/arm/cpu_common.c` | `ml_cpu_can_exit()`: cores 5-7 with `m3off=2` and `processor_exit=1` (every core was "required online" on SWEET) |
| `bsd/dev/hakios.exynos_cpufreq.c` | the policy, the test verbs, offline cores neither measured nor counted, the rule's inputs (below) |
| `bsd/dev/herolte_ufs.c` | `kern.sweetwdog_test` and its two ages get CTLFLAG_KERN: the USB console's `sysctl` could not reach them (EPERM) |
| scion-kernel `drivers/power/exynos/hakios.exynos_cpufreq_ceiling.h` | the rule's second way (below); `tools/exynos_cpufreq_ceiling_test.c` checks it (1439 checks) |

| Boot-argument | |
|---|---|
| `m3off=1` | the policy decides and logs ("would take cpus 7 and 6 offline now"); no core touched, the M3s at 1794 MHz |
| `m3off=2` | it runs; needs `processor_exit=1` (XNU's switch), `ocp_guard=1` (the PMIC warning read) and `cpufreq_gov_big_max=2314000` |
| `m3off_auto=0` | with `m3off=2`: no policy; only `kern.hakios.m3off_test` moves cores |

`kern.hakios.m3off_test` (from the USB console: `sysctl kern.hakios.m3off_test=N`): 1 cpu 7 offline 5 s and back; 2 cpus 7
and 6; 3 those with the M3s at 1924 MHz for the 5 s; 4 at 2314 MHz; 1N and 2N repeat 1 or 2 N times. `kern.hakios.m3off_stats`
has the counters. The console lines: `SWEET M3OFF: cpu N OFFLINE (XNU offline, CPU_OFF, PMU reads off) in U us`, `... BACK
(CPU_ON, running) in U us`, and once a second while anything happens `cpufreq_gov: m3off: S s: N off (want W), M3s online O,
big R kHz max M, sum P%, temp big T mC, PMIC warning on | to two A, back B, X ms at two`.

## (3) The policy and the rule above 1794 MHz

The rule (`hakios.exynos_cpufreq_ceiling.h`, the one every option goes through): with `m3off=2`, above 1794000 only while the
PMIC's CPU warning reads on, a fresh big-zone temperature is under 75 C, and at most two M3s are on, "on" meaning not taken
offline and read off by the PMU; then 2314000 at most (`dual_freq`). The governor's own online count (cores it measures) gives
the same 2314000-or-1794000 limit on top. While cores are being brought back the count is taken as four.

The policy (policy 1 only), every load window:

- **to two** (cpus 7 and 6 offline): up 120 s or more, a fresh temperature under 70 C, no thermal step, the PMIC warning on,
  the interface moving (a finger, or frames under 50 ms apart: an idle screen presents ten a second) and, three windows in
  a row, the M3s' load fitting on two (the third-busiest at 30% or less, the four's busy shares 150% or less); a second
  after the last change at least. The governor's load law then picks the rate, up to 2314 MHz.
- **back to four**: the interface still for a second, the two's shares adding up to 180% or more for five windows, or no
  longer eligible; half a second after the last change. The rate first: the return holds the M3s at 1794 MHz, and the cores
  are started only once the governor has decided a window with it and the rate reads under it (Samsung's `validate_hpin`).
- **stop** for the rest of the boot (a line `m3off: STOP: ...`, all four back, 1794 MHz at most): the big zone at 75 C while
  cores are offline or asked to be; a core XNU will not take or start; the rate not under 1794 MHz within 2 s of a return.

## (4) Safety: what ends a boot, and the watchdog

- A core the PMU still reads on 2 s after XNU had it offline, or one that runs again in XNU while the PMU does not say so:
  `panic`. A core that does not arrive after CPU_ON: XNU's own `processor_wait_for_start` panics. CPU_ON answering an error:
  `SweetCPU::startCPU` panics. With `sweetreboot` (on by default) a panic writes its log to the pmsg ring and resets into
  recovery, so none of these needs hands.
- A hang with interrupts still alive somewhere: `sweetwdog=60` (HANG-WATCHDOG.md). Both its cases were provoked on phone 2
  with this kernel before any core was powered down: user space silent (`kern.sweetwdog_test=1`) and the kernel's thread
  silent (`=2`); each time the phone was in TWRP 84 s later with the watchdog's line in pmsg. What no software watchdog
  covers: every core stopped with interrupts masked (the hardware watchdog has no driver here).

## (5) The phone runs (phone 2, 23edfb2021027ece, 24 November; iOS 27.2.1; the owner's root; USB-powered, 100%)

`msr.sh` (smooth-1124's phases) 420 s after SpringBoard (the boot's early slow mode is over by then): 20 home-screen page
flicks, 16 Settings list flicks, four Settings open/close; `swp` is a first set of flicks just before. fps from the
framebuffer's 5-s windows; "kernel" is the fork at `c2f63b2` (L6btm, the phones' kernel) or this branch.

| Boot | Kernel, line | swp | flicks | Settings list | open/close | big max | above 1794 |
|---|---|---|---|---|---|---|---|
| B0 | L6btm as found | 30.0 | 29.2 | 21.2 | 14.4 | 67.2 C | 0 |
| F1066 | L6btm, M3s pinned at 1066 MHz | (16.7, still slow mode) | 28.5 | 21.4 | 17.0 | 57.4 C | 0 |
| T0c | this branch, `m3off=1` (decisions only) | 30.8 | 28.5 | 20.7 | 18.4 | 66.3 C | 0 |
| C1 | this branch, `m3off=0` | (lost) | (13.7, slow mode: not valid) | 21.0 | 18.9 | 66.3 C | 0 |
| P1 | this branch, `m3off=2` (policy) | 31.0 | 29.8 | 21.5 | 18.3 | 72.6 C | flicks 7.7 of 15.2 s, list 5.3 of 11.2 s |
| P2 | this branch, `m3off=2` (policy) | (lost) | 28.4 | 21.5 | 13.9 | 74.5 C | flicks 6.3 of 13.2 s, list 8.8 of 17.3 s |

"(lost)": the phone's USB console lost most of its lines in the C1 and P2 boots (the framebuffer's lines with them); those
boots were measured on as they ran. C1's flicks fell in the boot's slow mode (the home screen first shown at the flicks:
WidgetRenderer 31%) and do not count. Open/close moves between 13.9 and 18.9 with and without the policy alike.
In the policy boots the APM echoed every rate asked (34 answers logged), the measured clock matched, and the stop at 75 C
was not reached (74.5 C at most, in the Settings list).

In F1066 backboardd took 59-62% (B0 47%) and SpringBoard 44-46% (B0 34-39%) of a core: the work grew as the clock fell, the
frame rate did not move. So the frames are paced by something other than the M3s' speed (the display path, the GPU queue,
the compositor's own pacing are the places left; `smooth-1124`'s model of SpringBoard's main thread needing 20-23 ms a frame
at 1794 MHz predicts about 20 fps at 1066 MHz, which is not what happened).

The tests (`m3off_auto=0`, one core and seconds first): test 1 (cpu 7, 5 s) once and five times: offline in 637-691 us, back
in 249-257 us; test 2 (cpus 7 and 6) once and five times: with two off the rule's ceiling went to 2314000 and the rate stayed
at 1794 (no load); test 3: 1924 MHz measured 1922-1924 for 5 s (the load law took it to 2002 and 2106 for a moment), back to
1794 MHz before the cores started; test 4: 2314 MHz measured 2313-2314 MHz for 5 s with two on, 44 C. In all, 22
offline/online cycles of a core in the test boot. The policy boots: 21 and 36 changes to two and back (95 and 80 s at two).

The dry run (`m3off=1`, T0c): 21 "would take" and 21 "would bring back" over the measured run, at most about one change a
second. Its first boot (T0) stopped the policy at 76 C during the boot's own heat with all four on and nothing asked; the stop
now latches only while the path is doing something.

Power: not measured. The phone was on USB at 100% (the gauge's current is the charger's); the temperature is the only cost
measured.

## (6) What it would take to turn it on, and what is not verified

It is off by default and should stay off: on this phone it did not make the interface faster. Before anyone turns it on:
a long soak (hours of use and idle with `m3off=2`, hundreds of cycles, both phones), a power measurement off USB, and a
reason (a workload whose frame rate does follow the M3s' clock). The offline path on its own (without the policy) is what
OCP.md's stage two and CPU-IDLE.md's cluster power-down were waiting for.

Not verified: that the cores' state after CPU_OFF and CPU_ON is complete for every use (the tests ran the interface, Wi-Fi,
Bluetooth and the camera daemon's start; not audio playback, not the modem under load); the M3 cluster's own power-down (one
core always stays); cores 5 and 6 at the same time as 7 more than 25 times; phone 1; the 75 C stop on a phone (never reached:
72.6 C at most); a trip of the PMIC's over-current warning (none seen; nothing reads it as an interrupt here).
