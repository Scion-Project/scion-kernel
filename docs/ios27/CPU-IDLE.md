# iOS 27 on the Galaxy S9: powering an idle core down ("c2") (17 November 2026; built, not run on a phone)

Under Scion an idle core of the S9 executes WFI and stays powered: no core is ever powered down, no cluster, and the phone
never suspends (scion-starlte `docs/hardware-map.md`, "Power: the largest gap"; its `README.md`, "The cores": "this kernel idles
with WFI and starts each core once"). Samsung's device tree has the numbers for a per-core power-down in idle, which the
device tree calls "c2". This page sets out what is known about it, from Samsung's device tree and kernel (read for facts; no
code of theirs is in the fork), how XNU's own idle path in the fork already returns a core that lost its context, the sequence
proposed for the S9, the rule that decides when, the risks with a phone test for each, and what cluster power-down, SICD and
suspend would need later.

Stage one is built in Scion-Project/xnu, branch `agent/scion-cpu-idle/c2` (`osfmk/arm64/sweet_c2.c`), behind `sweetc2=`,
off by default. Without the argument the idle path is the one it was: one load and a branch in front of the same WFI. It has
been built for STARLTE and FRANKEL and exercised in QEMU (FRANKEL, "QEMU" below), which proves the decision, the counters,
the PSCI call and the wake-up from core 0, and does not prove the return through the reset vector or anything about the
Exynos 9810. **No phone has run it.** A core that does not come back is a hang; the test plan below goes one core and one
entry at a time.

Line numbers in this page are those of `exynos9810-starlte_eur_open_26.dts` and the files of
`Scion-Project/android_kernel_samsung_exynos9810` at `lineage-24.0`, and of the fork at `scion/xnu-12377` commit `8bec662`
(before this change) unless a file is said to be new.

## Contents

- (a) The facts: Samsung's device tree and kernel
- (b) XNU's idle path in this fork, and what a powered-down core loses
- (c) The sequence for the S9, and what wakes a powered-down core
- (d) The rule
- (e) Risks, and the test plan
- (f) Later stages: cluster power-down, SICD, suspend
- The implementation, its boot-arguments and counters
- QEMU
- Not verified

## (a) The facts: Samsung's device tree and kernel

### The device tree

| What | Value | Where (`exynos9810-starlte_eur_open_26.dts`) |
|---|---|---|
| Cores 0-3 | `cpu@0000`..`cpu@0003`, "arm,ananke" (Cortex-A55), `reg` 0x0-0x3, `enable-method = "psci"`, `cpu-idle-states = <61>` (bootcl-cpu-sleep) | 4720-4762 |
| Cores 4-7 | `cpu@0100`..`cpu@0103`, "arm,meerkat" (Samsung M3), `reg` 0x100-0x103, `cpu-idle-states = <64>` (nobootcl-cpu-sleep) | 4764-4806 |
| A55 idle state | `bootcl-cpu-sleep`: `idle-state-name = "c2"`, `compatible = "exynos,idle-state"`, `arm,psci-suspend-param = <65536>` (0x10000), entry 70 us, exit 160 us, min-residency 2000 us | 4811-4821 |
| M3 idle state | `nobootcl-cpu-sleep`: "c2", 0x10000, entry 235 us, exit 220 us, min-residency 3500 us | 4823-4833 |
| PSCI | `method = "smc"`, `cpu_suspend = <0xC4000001>`, `cpu_off = <0x84000002>`, `cpu_on = <0xC4000003>`; compatible "arm,psci" only, i.e. PSCI 0.1: function numbers from the tree, no PSCI_VERSION | 4998-5004 |
| `exynos-pm` | suspend: `suspend_mode_idx = <8>`, `suspend_psci_idx = <133>`, `cp_call_*`, the wake-up status interrupts | 4932-4953 |
| `exynos-powermode` | `cpd_residency` and `sicd_residency` 10000 us, `cpd_enabled` and `sicd_enabled` 1, the `idle-ip` list (the blocks that must be idle for SICD), `idle_ip_mask/SYS_SICD`, four `wakeup-mask` registers (PMU offsets 0x610, 0x614, 0x618, 0x644) | 4955-4996 |
| GIC | `interrupt-controller@10100000`, "arm,cortex-a15-gic" (a GIC-400, GICv2): distributor 0x10101000, CPU interface 0x10102000 | 5029-5038 |
| Architected timer | "arm,armv8-timer", 26 MHz, `use-clocksource-only`, `use-physical-timer`; **no `always-on`** | 5040-5046 |
| MCT | `mct@10040000`, "samsung,exynos4210-mct", `use-clockevent-only`; twelve interrupts through `mct-map` to SPIs 466-477 (four global comparators, then eight per-core local timers) | 5056-5073 |

The PMU is `system-controller@14060000` (0x14060000, 64 KB); the interrupt generator the c2 sequence writes is at 0x14070000
(only in the CAL tables, below).

### The kernel's c2 path

1. `drivers/cpuidle/cpuidle-exynos64.c`: `exynos_enter_idle()` (80-100) calls `prepare_idle()` (41-53): `cpu_pm_enter()` (the
   notifier chain: GIC, timer, FP, debug) then `exynos_cpu_pm_enter(cpu, index)`; `enter_idle()` (66-78) calls
   `arm_cpuidle_suspend(index)`; `post_idle()` (55-64) calls `exynos_cpu_pm_exit(cpu, fail)` then `cpu_pm_exit()`, also when
   the entry failed.
2. `drivers/soc/samsung/exynos-powermode.c`: `exynos_cpu_pm_enter()` (521-559), under a spin lock: `cpu_disable(cpu)` (89-95:
   `cal_cpu_disable(linear_phycpu(mpidr))`), marks the core in `c2_mask`, and only if the next event is at least
   `min(cpd_residency, sicd_residency)` away tries CPD (`is_cpd_available`, 441-466: never the boot cluster, every core of the
   cluster in c2 and idle long enough) and SICD (468-491: boot cluster only, every core in c2, no idle-ip busy, no JIG cable).
   `exynos_cpu_pm_exit()` (561-583) is the reverse, starting with `cpu_enable(cpu)`. `linear_phycpu()` (69-74): with the MT bit
   (the A55s, MPIDR 0x81000n00) the physical number is Aff2*4 + Aff1 = n; without it (the M3s, 0x80000100+n) Aff1*4 + Aff0 =
   4 + n.
3. `cal-if/pmucal_cpu.c`: `pmucal_cpu_disable()` (52-79) writes the power-mode hint first (`pmucal_powermode_hint(cpu_inform_c2)`,
   68), then runs the core's `off` sequence; `pmucal_cpu_enable()` (16-42) clears the hint (32), then runs `on`. The hint
   (`pmucal_powermode.c` 21-37) is a write to the calling core's CPU_INFORM register, PMU + 0x860 + 4 x phycpu
   (`flexpmu_cal_powermode_exynos9810.h` 1-11); `CPU_INFORM_C2` is 1 (`flexpmu_cal_define_exynos9810.h` 2, `cal_data.c` 176).
4. The sequences, `exynos9810/flexpmu_cal_cpu_exynos9810.h` 1-263 (the `#ifndef ACPM_FRAMEWORK` half; `ACPM_FRAMEWORK` is not
   defined in `cal-if/Makefile`, `pwrcal-env.h` or `exynos9810/cal_data.c`, which is the half the kernel builds; the `#else` half,
   which writes `CLUSTERn_CPUm_CONFIGURATION` itself, is presumably the build of the same tables for the PMU's own firmware). For A55 n the bit is 4+n, for M3 n it is
   n; INTR = 0x14070000, PMU = 0x14060000:
   - `off` (e.g. `core00_off`, 9-13): INTR+0x200 `GRP2_INTR_BID_ENABLE` |= bit; if (INTR+0x108 & bit) == bit then INTR+0x10c
     `GRP1_INTR_BID_CLEAR` |= bit; PMU+0x7f08 `PMU_EVENT_INTERRUPT_ENABLE_GRP2` |= bit.
   - `on` (4-8): INTR+0x200 &= ~bit; if (INTR+0x208 & bit) == bit then INTR+0x20c `GRP2_INTR_BID_CLEAR` |= bit; PMU+0x7f08 &= ~bit.
   - `status` (14-16 and so on): `CLUSTER0_CPUn_STATUS` PMU+0x2024+8n, `CLUSTER1_CPUn_STATUS` PMU+0x2004+8n, bits 3:0 (0xF: on).
   Every write is a read-modify-write of the masked bits (`pmucal_rae.c` 116-126), a `COND_WRITE` happens only when the
   condition register masked equals the value (60-70, 245-247), and all of it runs under `pmucal_cpu_lock` (`cal-if.c` 232-252).
   What the bits do inside the PMU's microcontroller (APM) is in no document this project has.
5. `drivers/firmware/psci.c`: for a state that loses context (`psci_power_state_loses_context`, 113-120: the PSCI 0.2 StateType
   bit, 16) `psci_cpu_suspend_enter()` (475-495) goes through `cpu_suspend(index, psci_suspend_finisher)`, which saves the context
   and calls CPU_SUSPEND with the tree's parameter and the physical address of `cpu_resume` (415-420), context id 0. The other
   modes Samsung packs itself (`psci_power_state_pack`, 430-438; `psci_suspend_customized_finisher`, 446-473): cluster
   power-down = affinity level 1 (0x1000000), SICD = state id 1 (0x1), SICD with the cluster 0x1000001, CP call = level 2,
   system sleep = level 3 (0x3000000).
6. The GIC (`drivers/irqchip/irq-gic.c`): on `CPU_PM_ENTER` `gic_cpu_save()` (667-695) keeps the banked PPI enables, actives and
   configuration; on `CPU_PM_EXIT` `gic_cpu_restore()` (697-737) writes them back, sets every banked priority to the default, the
   priority mask to 0xF0 and the CPU interface up (notifier 739-760). So Samsung's kernel does not count on the banked state
   and the CPU interface surviving c2.
7. The timer: `drivers/clocksource/arm_arch_timer.c` reads `use-clocksource-only` (1048-1050: the architected timer is no
   clock-event device at all) and `always-on` (1053: without it the timer is marked `CLOCK_EVT_FEAT_C3STOP`, 485, "stops in
   deep idle"); on `CPU_PM_ENTER/EXIT` it saves and restores CNTKCTL (806-813). The per-core events come from the MCT
   (`drivers/clocksource/exynos_mct.c`): one local timer per core (509), its SPI bound to that core (526), and no C3STOP (516):
   an interrupt outside the core that wakes it. That is the design Samsung chose for c2.

## (b) XNU's idle path in this fork, and what a powered-down core loses

### The path a WFI takes today

- `machine_idle()` (`osfmk/arm64/machine_routines.c` 398-417), interrupts masked: unless a WFE recommendation is pending it
  calls `Idle_context()` (`osfmk/arm64/cswitch.s` 413-421), which saves the idle thread's callee-saved registers and stack
  pointer at the top of its kernel stack, moves to the core's interrupt stack and branches to `cpu_idle()`, which never returns.
- `cpu_idle()` (`osfmk/arm64/cpu.c` 481-650): the WFE paths, the platform's `cpu_idle_notify` (none on SWEET: `SweetCPU`
  registers no `processor_idle`, so `cpu_idle_notify` is NULL and `cpu_idle_latency` 0, `machine_routines.c` 1736-1741),
  `platform_cache_idle_enter()` (590; on STARLTE, built without `__ARM_COHERENT_IO__`, that is `FlushPoU_Dcache()`, a set/way
  clean and invalidate of the core's data cache before every WFI, `osfmk/arm/caches.c` 359-374), `DSB SY`, `WFI` (614), then
  `ClearIdlePop(TRUE)` (647: reprogram the timer to `rtcPop`) and `cpu_idle_exit(FALSE)` (649), which ends in
  `Idle_load_context()` (`cswitch.s` 430-437): the idle thread's registers back, and `machine_idle()` returns.

### The path back after a lost context: already in XNU

Apple's cores can lose their state in idle, and XNU has the return for it:

- After a core's cold start `arm_init_cpu()` calls `cpu_machine_idle_init(FALSE)` (`osfmk/arm/arm_init.c` 1046), which sets the
  core's `cpu_reset_handler` to the physical address of `resume_idle_cpu` (`cpu.c` 1300) and cleans `cpu_data` (1302). The
  boot core set `ResetHandlerData.cpu_data_entries` (1250).
- A reset enters `LowResetVectorBase` (`osfmk/arm64/start.s` 104) → `reset_vector` (127): unlock the OS lock, mask interrupts,
  set VBAR, find the core's `cpu_data` by comparing MPIDR's Aff1:Aff0 with each `cpu_phys_id` (on STARLTE bit 16 is added for
  the M3s, whose MPIDR has no MT bit, 147-157), and call its `cpu_reset_handler` if it is `resume_idle_cpu` or `start_cpu`
  (192-219); anything else hangs (`b .`).
- `resume_idle_cpu` (371) is `start_cpu` with `arm_init_idle_cpu` as the continuation: stacks from `cpu_data` (413, 427), the
  MMU, `TTBR1` from `cpu_ttep`, VBAR, SCTLR, PAN off on an A55 (928-938), CPACR (991), tunables.
- `arm_init_idle_cpu()` (`arm_init.c` 1134-1167): the current thread, `wfe_timeout_init()` (CNTKCTL, with EL0 access to the
  counter), user TTB cleared and TLBs flushed, the debug state dropped, `fiq_context_init(FALSE)` (CNTV enabled), and
  `cpu_idle_exit(TRUE)` (`configure_coresight_registers`, the idle notify), which ends in `Idle_load_context()`.

On SWEET the PSCI entry `sweet_secondary_entry` (`start.s` 324-366) already leads into this: it takes a core that arrives at
EL2 or EL1 with the MMU off, sets SPSel and branches to `LowResetVectorBase`. CPU_ON uses it (`iokit/Kernel/arm/SweetCPU.cpp`
398); so can CPU_SUSPEND. That is the per-core context mechanism stage one needed, and it did not have to be written.

What is Apple's and not used here: on Apple SoCs the platform kext enables power-gating in WFI through `cpu_idle_notify`
and the hardware loses the core by itself; `SetIdlePop()` (`osfmk/arm/rtclock.c` 412-434) programs the timer
`cpu_idle_latency` early to hide the exit. `arm64_prepare_for_sleep()` and `__ARM_GLOBAL_SLEEP_BIT__` belong to system sleep
(`cpu_sleep()`): on Apple's SoCs deep sleep is a global register, so every core must reach the sleep path before it is set,
or a core idling in WFI would hang (`cpu.c` 1154-1169). On SWEET system sleep is not wired (`SweetCPU::haltCPU` panics,
`SweetCPU.cpp` 454; `initCPU(false)` panics, 320).

### What PSCI CPU_SUSPEND with a power-down state loses, and who restores it

PSCI (DEN 0022) says the caller loses the core's context and resumes at the entry point given, MMU off, at the EL that
called; the firmware cleans the caches of the levels it powers down. For one core (affinity level 0, this parameter):

| Lost | Restored by |
|---|---|
| General and FP/SIMD registers | the idle thread's are saved by `Idle_context()` and loaded by `Idle_load_context()`; FP state is per thread and saved at switch |
| EL1 system registers: SCTLR, TTBR0/1, TCR, MAIR, VBAR, CPACR, TPIDR_EL1, TPIDR(RO)_EL0, CONTEXTIDR, SP_EL0/1 | `reset_vector`, `start_cpu`/`common_start`, `machine_set_current_thread()`; TPIDRRO_EL0 at the next switch to a user thread |
| PSTATE.PAN (A55) | `common_start` (`start.s` 928-938) |
| CNTKCTL_EL1 (EL0 counter access, event stream) | `wfe_timeout_init()` in `arm_init_idle_cpu()` |
| CNTV_CTL/CVAL (the timer XNU uses), CNTP | `fiq_context_init()` enables CNTV; **the compare value is not reprogrammed by Apple's path**: stage one calls `ClearIdlePop(FALSE)` |
| Debug registers, OS lock | `reset_vector` (OSLAR, 130), `arm_debug_set(NULL)` and `configure_coresight_registers()` |
| PMU (PMCR, counters) | nothing to restore: `CPU_COUNTERS_BASE` is empty for STARLTE (`config/MASTER.arm64.starlte` 15) and the only PMU use is `starlte_cpu_clock_report()` at boot (`bsd/dev/starlte_smp.c`) |
| The core's L1 caches, TLBs, branch predictor | the firmware cleans L1 (PSCI); TLBs flushed in `arm_init_idle_cpu()` |
| The GIC CPU interface (GIC-400, outside the cores: should survive; Samsung restores it anyway, above) | stage one re-runs the core's part of `pe_init_fiq()` and counts the times the state was found lost |

And what the reset path needs that a cache could hide: `reset_vector` and `start_cpu` read `CpuDataEntries`, the core's
`cpu_data` (`cpu_phys_id`, `cpu_reset_handler`, the stack tops), `BootArgs`, `cpu_ttep` and the boot page tables with the MMU
and caches off. `cpu_start()` cleans all of them to the point of coherency before CPU_ON (`cpu.c` 937-948, SWEET), but
`cpu_reset_handler` changes after that (to `resume_idle_cpu`). Stage one cleans every core's `cpu_data` to PoC when it arms,
and the core's own before each power-down. Without it a core could find the old `start_cpu` in memory and take the cold
start path a second time.

## (c) The sequence for the S9, and what wakes a powered-down core

### Entry (on the core, interrupts masked, in `cpu_idle()` in place of WFI: `sweet_c2_idle()`)

1. Decide (the rule, (d)). Read the next timer: `rtcPop`, and whether there is any timer at all (the three sources
   `timer_resync_deadlines()` takes: the rtclock queue, the idle timer, the running timers).
2. If a timer is pending: the wake-up deadline is `rtcPop` less the exit latency (160 or 220 us) and 100 us for the IPI. Bump
   the core's generation, mark it "in c2", and ask core 0 by cross-call (`SIGPxcall`) to arm a `timer_call` at that deadline on
   its own queue. If core 0 has a cross-call pending already, give up and WFI (counted as `busy`).
3. The Exynos writes Samsung makes (STARLTE): CPU_INFORM = 1, then the `off` bits, under a spin lock.
4. Clean the core's `cpu_data` to PoC; DSB.
5. PSCI CPU_SUSPEND: x0 = 0xC4000001, x1 = 0x10000, x2 = the physical address of `sweet_secondary_entry`, x3 = the logical CPU
   number (not used: the reset path goes by MPIDR). SMC on STARLTE (`SWEET_PSCI_CONDUIT`, `board_config.h` 335), HVC on FRANKEL.

### Exit

- **Back through the reset vector** (the normal case): `sweet_secondary_entry` → `LowResetVectorBase` → `resume_idle_cpu` →
  `arm_init_idle_cpu()` → `sweet_c2_resume()`: the `on` writes (CPU_INFORM 0, then the bits), "out of c2", cancel core 0's
  timer call, count; check the GIC CPU interface (GICC_CTLR enable, GICC_PMR, the banked enables of SGI 1 and INTID 27) and
  count when lost; `pe_init_fiq()` (the core's GIC set-up, idempotent); `ClearIdlePop(FALSE)`, which programs the virtual timer
  for `rtcPop` again (a deadline already past fires at once); then `cpu_idle_exit(TRUE)` and `Idle_load_context()` as before.
- **The call returns 0**: the firmware did not power the core down (a wake-up was pending; PSCI allows this). Nothing was
  lost: the `on` writes, cancel, count, and on as after a WFI.
- **The call returns an error** (-2 INVALID_PARAMETERS, -3 DENIED ...): the same, counted with the code; after 8 in a row the
  core gives c2 up for the boot.

### What wakes a powered-down core

- **Its IPIs (SGIs)**: XNU wakes an idle core with `cpu_signal(SIGPnop)`, an SGI through the GIC. That a pending interrupt
  for a powered-down core makes the PMU power it up is how Samsung's own c2 works (its IPIs and MCT local timers both wake c2
  cores); it is the one hardware fact stage one rests on that only a phone can confirm (test 3).
- **Device interrupts**: in this port SPIs are targeted at core 0, so the secondary cores receive only SGIs and their own
  timer PPI.
- **Its timer: assumed lost.** The tree does not mark the architected timer `always-on`, which in Linux's binding means it
  stops in power-down states, and Samsung does not use it for events at all (above). This port has only the architected
  timer (`A5XELTE_VTIMER_INTID` 27, `pexpert/pexpert/arm64/A5XELTE.h` 87; no MCT driver). Stage one's answer is a software
  broadcast: core 0, which never powers down, holds a `timer_call` per sleeping core and sends it an IPI before its timer is
  due; the woken core reprograms its own timer. This costs core 0 a cross-call per entry and a timer interrupt per wake-up;
  `sweetc2flags=1` avoids both by powering a core down only when it has no timer at all. The hardware alternative, the MCT's
  per-core local timers (SPIs 470-477 for cores 0-7 after the four global comparators), would need a driver for them, their
  clocks and their GIC routing; it is stage 1b, if the phone shows the broadcast costs too much.

Whether core 0's timer call keeps the wake-ups on time is measured: `core0 armed/fired` against the entries, and the woken
core's timer firing at once after `ClearIdlePop` would show a late wake-up as a late timer in kdebug (not added).

## (d) The rule

A core powers down when all of these hold, checked in this order (each refusal counted by reason):

1. c2 is armed (the self-check passed) and `kern.hakios.c2.enable` is 1;
2. the core is allowed: `sweetc2=1` the A55s 1-3, `sweetc2=2` cores 1-7; `sweetc2cpu=N` narrows it to one; **core 0 never**
   (it is the timer broadcast, and Samsung keeps the boot cluster up for CPD);
3. its own self-check passed (below), it has not given up after 8 failures, and `sweetc2max` is not reached;
4. no driver holds c2 off (`sweet_c2_block(1)`, for a driver that cannot wait an exit latency for its interrupt; none does
   yet: the secondary cores receive no device interrupts in this port);
5. with `sweetc2flags=1`, the core has no timer at all;
6. the next timer is at least the cluster's min-residency away: 2000 us for an A55, 3500 us for an M3 (the device tree's own
   break-even; Samsung's cpuidle uses the same via `target_residency`);
7. unless `sweetc2flags=4`, the core's previous idle lasted at least as long: the next timer bounds the idle time from above,
   and IPIs end most idles first. Linux's menu governor corrects the timer prediction by history in the same spirit; this is
   the simplest form of it, and the dry run measures how good it is.

The self-check, before arming (core 0's thread, `sweetc2delay` seconds after every core has started): the boot core is logical
0 at MPIDR 0; 2 to 8 cores, all running; the physical addresses of `sweet_secondary_entry` and `resume_idle_cpu`; EL1; PSCI:
PSCI_VERSION is NOT_SUPPORTED (the S9's PSCI 0.1) or a version, and from 1.0 PSCI_FEATURES says CPU_SUSPEND exists with the
original power_state format (in which 0x10000 means power-down); FRANKEL must answer a version; STARLTE: the PMU and the
interrupt generator map, and every running core reads 0xF in its `CLUSTERn_CPUm_STATUS`. On each core, the first time it
could power down: MIDR and MPIDR are what its logical number says (A55: 0x41/0xD05, Aff1 = core, MT; M3: 0x53/0x002, 0x100+n),
`cpu_reset_handler` is `resume_idle_cpu`, its PMU status reads on and its CPU_INFORM reads 0. A core that fails never powers
down; c2 that fails the system check is never armed. Both are printed.

## (e) Risks, and the test plan

### What can go wrong, and how a test would show it

| # | Risk | What the phone would show |
|---|---|---|
| R1 | The firmware rejects the call (the parameter, the entry address, PSCI 0.1's CPU_SUSPEND not implemented) | `failed N (last -2/-3/-1)` and `F` events; the core gives up after 8 and runs on in WFI. Harmless |
| R2 | The core powers down and never comes back (the SGI does not wake it, the PMU does not power it up, the hint or the interrupt-generator bits are wrong or missing) | `E` event with no `R`/`P` after it; that core's threads stall; anything waiting on a cross-call to it panics after XNU's xcall timeout ("CPU%u has failed to respond to cross-call"), or the phone hangs. With `sweetc2cpu=` one core, `sweetc2max=1` one entry |
| R3 | It comes back, but not through `resume_idle_cpu` (a stale `cpu_reset_handler` in memory: `start_cpu`; a null handler: `b .`) | a second "cpu_start"/`arm_cpu_init(): cpu N online` or a hang right after the `E`. The PoC clean exists for this |
| R4 | It comes back at EL2, or with x0..x3 not as PSCI says | `sweet_secondary_entry` handles EL2 and ignores x0; an unexpected EL hangs at `Lsweet_secondary_bad_el` |
| R5 | It comes back with the GIC CPU interface reset | `gic-lost` counter and `G` events (the bits say which); `pe_init_fiq()` restores it either way |
| R6 | The timer: late or missing wake-ups (core 0's timer call late, coalesced, or lost) | `core0 fired` well below `in` minus `back`; timers of the sleeping core late (audio glitches, UI stalls on that core). `sweetc2flags=1` removes the dependency |
| R7 | The timer is not lost after all (the comparator survives) | harmless: the core wakes by its own timer and core 0's IPI arrives to an awake core |
| R8 | A cross-call storm: core 0 interrupted twice per entry | core 0's load in `STARLTE TOP`, `busy` counts. The residency rule limits entries to one per 2 ms per core at most |
| R9 | The M3s: the S9+ Linux port saw an M3 not come back after CPU_OFF and a cluster power-down (scion-starlte README, "The cores") | the same as R2, on cores 4-7: test them last, one at a time, and only after the A55s are clean |
| R10 | The PMU bits are shared registers: a lost update between two cores entering at once | covered by the lock; a wrong bit would show as R2 |
| R11 | Something in the port uses per-core state that the cold start sets and the idle return does not (a register written in `SweetCPU::initCPU` or a driver's per-core set-up) | a fault or wrong behaviour only on the core that came back; a reviewer can check: `initCPU` sets no register (`SweetCPU.cpp` 316-343) and the drivers send their interrupts to core 0 |
| R12 | The PSCI call returns 0 every time (the firmware treats it as standby, as QEMU does) | `psci-returned` ≈ `in`, `back-by-reset` 0: no harm and no gain; the power measurement would show it |
| R13 | The idle prediction is poor: entries that end within the residency cost more energy than WFI | the dry run's `woke early` against `would` |

### The plan, from the safest step

Every step with the USB console up (`serial=3` as today) and the stock command line otherwise; each step only after the one
before it ran clean for its whole duration. The counters are on the console every `sweetc2log` seconds and in
`sysctl kern.hakios.c2.stats`.

1. **Measure only.** `sweetc2dry=1`. Nothing is powered down. Read `STARLTE C2 ... dry:` after a few minutes idle, a few
   minutes of use and a video: the histograms say how much idle time on each core lies in periods longer than 2 ms
   (A55) and 3.5 ms (M3), and how often the rule's prediction is wrong. This sizes the gain before any risk is taken.
2. **Self-check only.** `sweetc2=2 sweetc2flags=8`: c2 arms, every allowed core runs its own check the first time it idles, and
   nothing is powered down. Expect the `armed` line and `event cpuN C 0` for cores 1-7 (an `X` names the fact that failed). This
   proves the PSCI answers, the PMU map and the per-core facts on the phone.
3. **One A55, one entry.** `sweetc2=1 sweetc2cpu=3 sweetc2max=1`. Expect `event cpu3 E`, then `event cpu3 R <us>` (back through the
   reset vector) or `P` (the call returned). The phone must go on working on all eight cores for ten minutes. If it hangs after
   the `E`: R2/R3; take the console log, nothing else is at risk.
4. **One A55, ten entries, then unlimited.** `sweetc2max=10`, then `sweetc2max=0`, same core. Watch `back-by-reset`, `gic-lost`,
   `unexpected`, `core0 fired`; use the phone normally on the console for an hour.
5. **The three A55s.** `sweetc2=1`. A day of normal use; then the power measurement (battery drain overnight with the screen
   off, against a night with `sweetc2=0`) and the temperatures (`kern.hakios.temp`).
6. **One M3.** `sweetc2=2 sweetc2cpu=4 sweetc2max=1`, then `sweetc2max=0`. If the M3 does not come back (R9), the M3s stay out:
   `sweetc2=1` is the end of stage one.
7. **All seven.** `sweetc2=2`.
8. Only then: `sweetc2flags=4` (no history guard), to compare entries and power.

If a step fails, the boot-argument goes; nothing persists across boots.

## (f) Later stages: cluster power-down, SICD, suspend

Not implemented; what each would need on top of stage one:

- **Cluster power-down (CPD)**, M3 cluster only (Samsung never powers down the boot cluster, `exynos-powermode.c` 458): all four
  M3s in c2 with their next events at least `cpd_residency` (10 ms) away; the last core to go down runs the cluster's `off`
  sequence (`cluster1_off`: `MAILBOX_APM2AP` bit 24, PMU 0x14100000 + 0x10, in the `!ACPM_FRAMEWORK` half), hints CPU_INFORM_CPD (2),
  and calls CPU_SUSPEND with affinity level 1 (0x1000000) instead of 0x10000; the first core up runs `cluster1_on`. Samsung also
  calls `big_reset_control()` around it, and `CPU_CLUSTER_PM_ENTER/EXIT` save and restore the GIC distributor. Needs: a
  per-cluster count of sleeping cores under one lock, the M3 behaviour of R9 understood, and the M3 PMU defaults that CPU_ON needs
  (`SweetCPU.cpp` `starlte_m3_pmu_defaults`) checked after a cluster power-down.
- **SICD** (system idle clock down), boot cluster only, every core in c2 for at least `sicd_residency` (10 ms), and every
  `idle-ip` block idle: the PMU's IDLE_IP registers (PMU + 0x3E0 + 4i) against the masks (0x3F0 + 4i), each driver reporting its
  block busy or idle (`exynos_update_ip_idle_status`); the wake-up masks of the tree; `cal_pm_enter(SYS_SICD)`; state id 1 in the
  call. That means core 0 powering down too, so the timer broadcast of stage one no longer works: SICD needs the MCT (stage 1b)
  first, and the idle-ip reporting in every Scion driver of the list.
- **Suspend**: `exynos-pm` mode 8, PSCI index 133, i.e. CPU_SUSPEND with affinity level 3 (0x3000000) on the last core after the
  others are off (CPU_OFF), with the EINT and GIC wake-up masks programmed, the PMIC and every device suspended, and a resume
  path for the whole SoC (`cpu_sleep()`/`arm64_prepare_for_sleep()` and `SweetCPU::haltCPU`, both unimplemented). A project of
  its own, after c2 and CPD are known good.

## The implementation, its boot-arguments and counters

Scion-Project/xnu, branch `agent/scion-cpu-idle/c2`, based on `scion/xnu-12377` at `8bec662`:

| File | What |
|---|---|
| `osfmk/arm64/sweet_c2.c`, `sweet_c2.h` (new) | everything above: the rule, the PSCI call, the PMU writes (STARLTE), core 0's timer calls, the resume, the self-check, the counters, the dry run, the log thread |
| `osfmk/arm64/cpu.c` | in `cpu_idle()`, the WFI becomes `sweet_c2_idle()` when `sweet_c2_on` is set (STARLTE and FRANKEL only) |
| `osfmk/arm/arm_init.c` | `arm_init_idle_cpu()` calls `sweet_c2_resume()` before `cpu_idle_exit(TRUE)` |
| `pexpert/arm/pe_fiq.c` | `a5xelte_gic_cpu_lost()` (STARLTE): which GIC settings of the core are gone |
| `iokit/Kernel/arm/SweetCPU.cpp` | `startAll` calls `sweet_c2_start()` once every core is up |
| `bsd/dev/starlte_c2.c` (new) | `kern.hakios.c2.stats` and `kern.hakios.c2.enable` |
| `osfmk/conf/files.arm64`, `bsd/conf/files` | the two new files |

| Boot-argument | Meaning |
|---|---|
| `sweetc2=N` | 0 (default) off; 1 the A55s 1-3; 2 cores 1-7 |
| `sweetc2cpu=N` | only logical CPU N |
| `sweetc2max=N` | at most N power-downs per core (0: no limit) |
| `sweetc2dry=1` | measure only (works with `sweetc2=0`) |
| `sweetc2delay=S` | arm S seconds after every core is up (default 60) |
| `sweetc2log=S` | print the counters every S seconds (default 30; 0: none) |
| `sweetc2flags=B` | 1: only idles with no timer; 2: none of the Exynos PMU writes (diagnostic); 4: no history guard; 8: self-check only, never power down |

These are arguments of the iOS 27 fork, which `tools/boot-args.py` does not scan (it reads the 16.5 profile's fork), as for
`sweetm3unpin`; they are listed here.

The counters, per core (`STARLTE C2: cpuN ...` on the console, the same in `kern.hakios.c2.stats`): `in` power-downs asked
for; `back-by-reset` returns through the reset vector; `psci-returned` calls that returned 0; `failed` calls that returned an
error, with the last code; `asleep` time from the call to the return, in ms; `core0 armed/fired` core 0's timer calls for the
core and the IPIs they sent; `busy` entries given up because core 0 had a cross-call pending; `gic-lost`; `unexpected` resets
with no entry recorded; `refused` by the self-check, and by rule (`short`, `history`, `timed`, `blocked`, `limit`). With
`sweetc2dry=1`: idles and their total time; the next timer at idle entry in buckets <1, 1-2, 2-3.5, 3.5-10, 10-100, >100 ms and
"none"; the time each idle lasted in the same buckets; `would` (the rule ignoring which cores are allowed) with their time, and
how many of those ended within the residency. The first events of each core are printed as `event cpuN K value`: `E` entered
(us to the next timer, -1 none), `R` back by reset (us asleep), `P` the call returned (us), `F` failed (code), `U` unexpected
reset, `X` refused by the self-check (bits: 1 ids, 2 reset handler, 4 PMU status, 8 CPU_INFORM), `C` the core's self-check
passed (`sweetc2flags=8`), `G` GIC lost (bits).

With none of `sweetc2` and `sweetc2dry` on the line, `sweet_c2_start()` returns at once, `sweet_c2_on` stays 0 and `cpu_idle()`
executes the same WFI (and `arm_init_idle_cpu()` is not reached, as before: no core resets in idle).

## QEMU

FRANKEL kernel of this branch in QEMU 11.1.3 (`-M virt,virtualization=on,gic-version=4 -cpu max -smp 8`, TCG, the lga boot shim
at EL2, xnuloader `adt_layer=s9`, no root device, so the kernel idles until it gives up waiting for one after about 150 s), with
`cpus=8 sweetc2=2 sweetc2dry=1 sweetc2delay=20 sweetc2log=10` and the four timing-check switches the Pixel 10 runs use under TCG
(`sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0 sched_hygiene_debug_pmc=0 ml_io_timeouts_enabled=0`):

- the self-check passed and armed: `PSCI_VERSION 65537` (1.1, through the shim's HVC), CPU_SUSPEND with the original format,
  entry and `resume_idle_cpu` physical addresses found;
- in the last report, about 80 s after arming, cores 1-5 had made 35 to 60 power-down calls each (the lines of cores 6 and 7
  were cut off by a print buffer that was too small, since doubled); every call that had ended returned 0 (QEMU's PSCI does
  CPU_SUSPEND as a WFI: "Powerdown is not supported, we always go into WFI"), none failed, none was unexpected; core 0 never
  entered;
- core 0's timer calls: armed for the entries that had a timer (most had none: in an idle kernel the secondary cores mostly
  have no timer at all; core 1's dry line: 77 idles in 79 s, next timer "none" for 58 and over 100 ms for 18, `would` 69, woke
  early 5), and a few fired the IPI that woke the core (core 1: 19 armed, 2 fired; core 3: 25 and 1); the others were cancelled
  because the core woke first (in QEMU the core's own timer still works, and the host was loaded, so wake-ups were late by tens of
  ms either way);
- the dry run's histograms and the `would` counts filled as described; the run ended in the same "Failed to mount root device"
  panic as any run without a root;
- without the arguments the same kernel printed no `SWEET C2` line and stopped at "Waiting on ... IOMedia" as before.

What this proves: the code is reached and is safe on the path where PSCI returns; the PSCI call reaches the firmware with these
registers; the rule, the counters, the cross-call to core 0 and its timer calls and IPIs work in a running XNU with eight cores;
the default path is unchanged. What it does not prove: the return through the reset vector (QEMU never loses the core),
anything of the Exynos 9810 (PMU, GIC-400, Samsung's EL3, the M3s), and the timing of real wake-ups.

## Not verified

- That Samsung's EL3 powers a core down for CPU_SUSPEND 0x10000 when called by this kernel (PSCI 0.1, function from the tree)
  and that it enters the given entry point at EL1 (the CPU_ON path suggests so; `sweet_secondary_entry` takes EL2 too).
- That an SGI wakes a powered-down core on this SoC with the interrupt-generator bits as Samsung sets them (inferred from Samsung's
  own c2, which relies on IPIs and MCT SPIs). What the APM firmware does with CPU_INFORM and those bits is not documented here.
- That the architected timer is lost in c2 (assumed; if it survives, the broadcast is only redundant).
- That the GIC-400's CPU interface keeps its state (counted on the phone; restored either way).
- That nothing else of this port depends on per-core state set only at cold start (reviewed: `SweetCPU::initCPU` writes no
  register; the secondary cores receive no device interrupts).
- The M3s (R9).
- The gain: no measurement exists until test 1 (the dry run) and test 5 (battery against a night without c2).
