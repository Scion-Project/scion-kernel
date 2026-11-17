# The Galaxy S9's big cores above 1794 MHz: Samsung's over-current protection, and Scion's guard (17 November 2026; built, not run on a phone)

The four Samsung M3s of the Exynos 9810 are held at 1794 MHz in Scion. That is the highest rate Samsung's kernel allows them
without its over-current protection, and with three or four of them online. This page sets out what Samsung's kernel does
before it lets them go faster, from its source (read for facts; no code of it is in Scion), what of that is firmware and
not known, the design that reproduces it in Scion, what is built (`ocp_guard=`, off by default, in both the iOS 16.5 driver
and the iOS 27 port), and the phone tests in order.

**The verdict first.** The protection splits into three parts. Two of them can be built: the OCP block's interrupt, its
step-down and release, and the IRP kept to the rate; and the PMIC's warning, which can be read and must already be on. They
are built here as "stage one", with the M3s still held at 1794 MHz. The third part cannot be built yet: Samsung runs 2314 MHz
only with the other M3s *powered off by CPU_OFF*, and this kernel cannot take an M3 offline. A parked M3 (idle in WFI) or one
powered down in idle (c2) is still online in Samsung's sense. So "stage two", the rule that would allow 2314 MHz, is built
and **refuses**, and will go on refusing until an M3 can be taken offline and started again. Also unproven: whether the
OCP block's registers can be reached from Scion's kernel at all. On an S9+ under iOS 16.5 the first read of one never came
back (kernel commit dab99986). Until a phone shows both, running above 1794 MHz cannot be made as safe as Samsung's own
kernel makes it.

Line numbers are those of `Scion-Project/android_kernel_samsung_exynos9810` at `lineage-24.0` (the device tree
`arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts`, "the tree" below), unless another file is named.

## Contents

- (1) The facts
- (2) The design
- (3) The implementation: `ocp_guard=`
- (4) The phone tests
- (5) What is not built, and why
- Not verified

## (1) The facts

### The OCP block: where it is and what it can be told

The block is the big cluster's own, reached only through **implementation-defined system registers** of an M3, not memory.
Samsung's driver (`drivers/soc/samsung/exynos-ocp.c`) names five of them and uses three:

| Register | Encoding | Fields the driver names | Driver's use |
|---|---|---|---|
| OCP_THROTTLE_CONTROL (`OCPTHROTTCTL`) | `S3_0_C15_C12_7` | [2] TEW, [7:3] TRW, [20:16] BPC, [23:21] TCRST, [26] OCPBPCHIT, **[27] OCPTHROTTERRA** | bit 27 read (`check_ocp_interrupt`, 304-312) and cleared by writing it 0 (`clear_ocp_interrupt`, 314-321) |
| GCU_CONTROL_REGISTER (`GCUCTL`) | `S3_1_C15_C11_0` | [25] OCPBPCEN, **[24] OCPTHROTTERRAEN** | bit 24 cleared and set around the handling (`control_ocp_interrupt`, 290-302) |
| OCP_INTEGRATION_CONTROL (`OCPINTCTL`) | `S3_0_C15_C12_5` | **[9:4] IRP** | written before every rate change (`exynos_ocp_irq_work`, 394-407) |
| OCP_THROTTLE_COUNTER_ALL (`OCPTHROTTCNTA`) | `S3_0_C15_C14_4` | [0] TDCEN, [31:20] TDT | never touched |
| OCP_INTEGRATION_T_INDEP_BOTTOM_POWER_THRESHOLD (`OCPTIDEPBOTTHRESH`) | `S3_0_C15_C15_1` | [23:0] BPT | never touched |

(29-66 for the encodings and fields.) All of it is done on one core, the first of `sibling-cpus = "4-7"`: the interrupt's
work is queued there (`schedule_work_on(data->cpu, ...)`, 364) and so is the IRP (`irq_work_queue_on(..., data->cpu)`, 422).

**What it measures and at what threshold: not in the source.** The names say an integrating power or current estimator in
the cluster ("integration", a "temperature-independent bottom power threshold", a throttle counter), but the driver never
writes a threshold, a throttle enable or a counter, and nothing else in the kernel does. Their values are whatever the
firmware (S-Boot, the EL3 monitor, or the APM) set. Neither is the interrupt enable turned on by the driver at its start
(`exynos_ocp_probe`, 665-730, only registers the interrupt): it is expected on already. So the block is configured and armed
by firmware, and Samsung's kernel only follows it.

### The interrupt, and what the driver does on it

- Device tree, `exynos-ocp` (5006-5015): `interrupts = <0 181 0>` (SPI 181, INTID 213), `sibling-cpus = "4-7"`, `down-step = <1>`,
  `max-freq-wo-ocp = <1794000>`, `release-mode = <1>`, `release-threshold = <50>`, `release-duration = <15>`.
- Requested rising-edge (`IRQF_TRIGGER_RISING`, 707-708). The handler only queues the work on cpu 4 (362-367).
- The work (`exynos_ocp_work`, 326-346): interrupt off (GCUCTL[24] = 0); if OCPTHROTTERRA is set, clear it and **lower the M3s'
  ceiling by `down-step` levels from the last ceiling** (`set_ocp_max_limit`, 155-187; `get_ocp_target_max_limit`, 138-153:
  from `clipped_freq`, not from the rate, so trips in a row go on down the table with no floor at 1794 MHz); interrupt on again;
  the release scheduled `release-duration` (15 ms) later.
- The ceiling is applied through the cpufreq policy (`exynos_ocp_policy_callback`, 373-388, at CPUFREQ_ADJUST), i.e. at once:
  `cpufreq_update_policy` brings a faster rate down.
- The release (`exynos_ocp_work_release`, 348-360): with `release-mode = 1` (CPU_UTIL_MODE) the condition is that the summed
  utilisation of cpus 4-7 is under 50% of their summed capacity (`is_cpuutil_condition`, 216-231; four cores, so a summed load
  under 200%). If it holds the ceiling goes straight back to the maximum (one step down, all the way up); if not it looks again
  15 ms later. (Mode 0, not used on this phone, reads BUCK2's current from the PMIC's ADC instead: 199-209.)
- Nothing reaches the user: a trip is logged to Samsung's debug snapshot (`exynos_ss_printk("OCP_enter:%ukHz")`, 177-181) and
  counted in sysfs (`total_trans`, `time_in_state`).

### The IRP, before every rate change

On every CPUFREQ_PRECHANGE of the big cluster the driver writes OCPINTCTL[9:4] = min(63, 2 x new rate / min-freq - 2)
(394-425), with min-freq the policy's (650000, domain@1's `min-freq`, 10569). So 650 MHz is 0, 1794 MHz is 3, 2106 MHz is
4, 2314 MHz is 5. That a period scales with the clock suggests the estimator integrates over a number of cycles, kept to
a constant time; what IRP means exactly, and what a wrong IRP does, is not known. The driver queues the write to cpu 4
and does not wait for it, so the rate may change a little before the IRP does.

### The PMIC's over-current warning

- `drivers/regulator/s2mps18.c` writes OCP_WARN1 (0xD3) at every boot: `en << 7 | reset << 6 | level` (1235-1248), from the
  PMIC node's `cpu_ocp_warn_en = <1>`, `cpu_ocp_warn_reset = <0>`, `cpu_ocp_warn_lv = <0>` (the tree, 8747-8749): 0x80. OCP_WARN2
  (0xD4) is the GPU's (`gpu_ocp_warn_en = <1>`, `gpu_ocp_warn_reset = <1>`: 0xC0). Register and shifts:
  `include/linux/mfd/samsung/s2mps18-private.h` 171-172, 280-282. BUCK2 is `vdd_cpucl1`, the M3s' rail (the tree, 8779).
- The OCP driver turns the warning off only when its own operation is turned off by hand, and then first clips the M3s to
  `max-freq-wo-ocp` (`control_ocp_operation`, 245-284). That is the pairing Samsung keeps: **warning on and OCP running, or the
  M3s at 1794 MHz at most.**
- Where the warning goes inside the SoC is not in the source (presumably to the cluster's throttle).
- The PMIC's own buck over-current interrupts (INT5 bits 0-7 for BUCK1-8, `s2mps18-private.h` 394-401; BUCK2 is bit 1) are only
  counted and logged by Samsung ("BUCK OCP IRQ", `s2mps18.c` 882-913) and reported to its HQM service; no action.

### What Samsung requires before it leaves 1794 MHz

1. The M3s' policy: `max-freq = <2314000>`, `policy-max = <1794000>`, `min-freq = <650000>` (domain@1, 10564-10571). The
   policy starts at 1794 MHz (`set_policy`, `drivers/cpufreq/exynos-acme.c` 815-826); levels above `max-freq` are left out of the
   table (`init_table`, 863-864): **2314 MHz is the highest rate this phone's kernel ever asks for.**
2. The OCP driver probed and its operation enabled (`data->enabled = true` at probe, 689): otherwise the ceiling is
   `max-freq-wo-ocp`, 1794 MHz.
3. The PMIC's CPU warning on (above).
4. The number of M3s **online**: `hotplug_governor` (10463-10479): `single_freq` and `dual_freq` 2314000, `triple_freq` and
   `quad_freq` 1794000. `drivers/soc/samsung/exynos-hotplug_governor.c`:
   - the policy's maximum by the count of M3s in `big_cpu_mask` (`get_hpgov_maxfreq`, 172-186, applied at CPUFREQ_ADJUST,
     188-207). A core enters that mask at CPU_UP_PREPARE and leaves it at **CPU_DEAD** (`exynos_hpgov_hp_callback`, 241-293), i.e.
     after it has gone through CPU_OFF;
   - every rate request is checked against the count in `cpu_online_mask` (`exynos_hpgov_validate_scale`, 124-145, called by
     the cpufreq driver before each change, `exynos-acme.c` 240): a rate above the mode's maximum is refused;
   - an M3 is not brought online while the rate is above what one more core allows (`exynos_hpgov_validate_hpin`, 147-167):
     the rate comes down first.
5. The voltage: the APM's own, from its table (the "fvmap", ASV voltages); the kernel sends rates only
   (`drivers/soc/samsung/cal-if/acpm_dvfs.c`). Scion's governor already reads that table and asks only for its levels; s3a's
   table gave 856 mV at 1794 MHz and 1012 mV at 2314 MHz (`docs/ios27/PERFORMANCE.md`, item 8).
6. The memory bus: the M3s' `mif-perf` constraint asks MIF 1794000 or more for every M3 rate from 1794 MHz up (domain@1's
   table, 10584): nothing more at 2314 than at 1794. (Scion asks for no MIF rate at all today: a performance matter,
   `PERFORMANCE.md`, not a safety one.)

### What "single" and "dual" mean, precisely

- The hotplug governor asks for a number of cores online: `req_cpu_min = mode + 4` for SINGLE (5) and DUAL (6) (333-352),
  through a PM QoS request with FAST_HP (359-382). `drivers/soc/samsung/exynos-cpu_hotplug.c` takes the cores of
  `fast_hp_cpus = "5-7"` (the tree, `cpu_hotplug`, 10457-10461) down together with `cpus_down()` (`do_cpu_down`, 203-221;
  `kernel/cpu.c` 1283-1300). Cpu 4 is never taken down (`fast_hp_cpus` leaves it out; `kernel/cpu.c` 1272: "One big cluster CPU and one little cluster CPU must remain online").
- Down is real hot-unplug: the core leaves `cpu_online_mask`, migrates everything off, and calls PSCI **CPU_OFF** with a
  power-down state (`arch/arm64/kernel/psci.c` 70-80; `cpu_off = <0x84000002>`, the tree's `psci` node, 4998-5004). The PSCI
  here is 0.1 (compatible "arm,psci" only): it has no AFFINITY_INFO, so `cpu_psci_cpu_kill` returns at once without
  confirming the core is off (86-87). Samsung trusts CPU_OFF.
- In SINGLE and DUAL the cluster's own power-down is blocked (`block_cpd(req_cpu_min != NR_CPUS)`, 298-301).
- So: **a core counts until it has been taken offline and has called CPU_OFF.** An M3 idle in WFI counts. An M3 in cpuidle's
  "c2" (CPU_SUSPEND power-down) is still online and counts: any interrupt brings it back at the cluster's rate.

### What happens on real phones when it trips

Not in the source beyond the above: the hardware throttles (by its names, and by the driver's only stepping down after the
fact), the driver lowers the ceiling one level for at least 15 ms, nothing is shown to the user. This project has no
field data of trips on an S9. The resets that matter are the ones without the protection: on an S9+ under iOS 16.5 the
M3s at 2704 MHz on all four cores reset the phone (builds 14 and 15, kernel commit dab99986); that is far past anything
Samsung allows.

### What is firmware, and not known

- The block's configuration: its thresholds (BPT, TDT, BPC), its throttle enables (TEW, TRW), its interrupt enable. Set by
  firmware before the kernel; restored after a core is powered down and up by firmware too, presumably (Linux never
  restores them).
- What the block measures, how fast it throttles, by how much, and what IRP means.
- Where the PMIC's OCP_WARN signal goes.
- Whether S-Boot leaves OCP_WARN1 at 0x80 when no Samsung kernel writes it (Scion reads it; see the tests).
- Whether the OCP registers can be read from Scion's kernel at all (below).

### What Scion has seen

- 16.5, S9+ (Anan Jaser, 12 November): `cpufreq_ocp=1` writes OCP_WARN1 = 0x80 and lets the M3s go to 2314 MHz with one or two
  of them *busy* and 1924 or 2106 MHz with more (Apollo's limits; `drivers/power/exynos/hakios.exynos_cpufreq.c`, its OCP
  note); `cpufreq_ocp_regs=1` read the OCP registers on an M3 and **the governor's thread never came back from the first read**
  (builds 14-19), which left the M3s at their starting rate, 105 C at boot (commit dab99986). Why is not known: a trap to a
  higher exception level that never returns would look like that.
- 16.5 and 27, S9: `gov_policy=3` runs two M3s at 2314 MHz with the other two parked in WFI. Neither of those is Samsung's
  protection: in Samsung's terms all four are online.

## (2) The design

The guard runs beside the governor: a thread of its own bound to cpu 4 (Samsung's `data->cpu`), so that if the OCP
registers do not answer, the governor, and with it the thermal ceilings, keeps running on its own thread.

### (a) The OCP block and its interrupt

- SPI 213 attached rising-edge, as Samsung's: iOS 27 through `starlte_gic_spi_attach()` (`pexpert/arm/pe_fiq.c`), 16.5 through
  `gic_v3_spi_attach()`. The handler, on the boot processor in interrupt context, counts, stamps and wakes the guard's thread;
  the registers are read on cpu 4 only.
- The thread does Samsung's work exactly: GCUCTL[24] off; OCPTHROTTERRA read; if set, cleared and the M3s' ceiling one level
  under the last ceiling; GCUCTL[24] on and **read back**; 15 ms later, the release if the M3s' summed load is under 200% (the
  governor's own per-window sum), else 15 ms more. Each trip is logged (`THROTTLE`, its count, the flag, the ceiling's step)
  and counted; the governor is woken at once and brings the rate under the new ceiling (as `cpufreq_update_policy`).
- The IRP: before every rate change of the M3s the governor asks the guard's thread for the new IRP; the thread writes it and
  reads it back. At or below 1794 MHz the change does not wait for it (Samsung's order); above 1794 MHz it waits up to 5 ms for
  the IRP to read back and otherwise goes no higher than 1794 MHz.
- The PMIC's OCP_WARN1 **read** (never written) at start and every ten seconds: bit 7 must be set. If S-Boot leaves it off,
  this kernel does not turn it on; the guard is then not armed. (Turning it on is a PMIC write, which the owner has not
  allowed; 16.5's `cpufreq_ocp=1` does it and is a separate, existing option.)

### (b) The rule for a rate above 1794 MHz

The M3s' ceiling, worked out every load window (20 or 40 ms) and after every guard event, is 1794 MHz unless **all** hold:

1. `ocp_guard=4`;
2. stage one armed in this boot (the registers read, the interrupt enable found on, the self-test passed, the PMIC warning on)
   and alive (its thread ran in the last second, nothing latched dead);
3. a fresh temperature of the big zone, under 83 C (BIG's control point);
4. no more M3s on than Samsung allows for the rate: at most two for 2314 MHz, "on" meaning not taken offline by CPU_OFF and
   read off by the PMU (part (c));

and then it is 2314 MHz at most (`dual_freq`; never any other rate above 1794 MHz than the table's levels up to it), under
every existing ceiling (thermal steps, 96 C cut, the OCP clip). When any of them stops holding the ceiling is 1794 MHz in the
same window, and the governor drops a faster rate at once (it does not wait out its down windows for a rate above its
ceiling). Stage two logs its decision once per change, with the reason.

### (c) What "offline" must be, and the dependency

Samsung's rule is about cores that are *offline*: taken out of the scheduler and powered off by CPU_OFF, and counted until
then. Neither of Scion's present mechanisms is that: parking (gov_policy=3) leaves the core in WFI, powered and able to take
an interrupt or a bound thread; the c2 power-down of `osfmk/arm64/sweet_c2.c` (`docs/ios27/CPU-IDLE.md`) powers the core off
only until its next interrupt. So stage two needs an M3 offline path in the fork:

- XNU's own processor shutdown (`processor_exit` and the platform's halt) wired to PSCI CPU_OFF on the core itself (today
  `SweetCPU::haltCPU` panics, CPU-IDLE.md (b));
- its start again through CPU_ON and `sweet_secondary_entry` (`osfmk/arm64/start.s`), the path c2 also depends on, which has not
  run on a phone. Starting an M3 again after CPU_OFF failed on the S9+'s mainline port
  (`drivers/soc/exynos/hakios.exynos_m3.c`'s head; CPU-IDLE.md's R9);
- the core confirmed off by the PMU (CLUSTER1_CPUn_STATUS, PMU + 0x2004 + 8n, [3:0] 0: already read here each ten seconds);
- the rate first: an M3 brought back only after the rate is at or under what one more core allows (`validate_hpin`).

`ocpg_m3s_off()` is where that path will say how many M3s are off; until it exists it is 0 and stage two refuses with
"more M3s on than Samsung allows above 1794000 (no M3 can be taken offline in this kernel)".

### (d) The watchdog

Any of these latches the guard dead for the rest of the boot (stage two never again; stage one goes on doing what it can),
with one line saying which:

- the self-test fails: SPI 213 is pended at the GIC by software (GICD_ISPENDR) at arming and every minute, and the handler
  must run within 100 ms. This proves the GIC's routing, the enable, the handler and the thread; it cannot prove that the
  block itself raises the line;
- the flag polled set (once a second, on cpu 4) with no interrupt in the last 50 ms: the block's side of the line is dead;
- GCUCTL[24] read back off; OCPINTCTL's IRP not reading back; the PMIC's warning read off.

The guard's thread not running for a second (it wakes at least every 250 ms) also ends stage two, without latching.

### How a trip can be seen at 1794 MHz, safely

Samsung runs 1794 MHz without the protection, so at 1794 MHz a trip is not expected and must not be provoked: the source
shows no threshold whose lowering is harmless (the driver never writes one), and raising the current is out. What can be
seen safely: the self-test (the path from the GIC on); the flag and OCPTHROTTCNTA polled each second (a throttle the block
did on its own would show there); and, with `ocp_guard_flags=1`, the PMIC's INT5 bit 1 (BUCK2 at its current limit). None
proves that the block raises its interrupt. That part can only be seen when a trip happens, which is a stage-two event.

## (3) The implementation: `ocp_guard=`

In `bsd/dev/hakios.exynos_cpufreq.c` of the fork (iOS 27, STARLTE only) and `drivers/power/exynos/hakios.exynos_cpufreq.c`
(16.5), the same code; a few lines differ (the SPI and PMIC calls, the cluster bits of a core's id). Without `ocp_guard` none
of it runs and the governor is what it was.

| Boot-argument | |
|---|---|
| `ocp_guard=1` | observe: OCP_WARN1/2 read, the M3s' PMU power status read, SPI 213 attached and counted, the self-test. No M3 register touched. The M3s at 1794 MHz at most whatever else is asked (`gov_policy=3`, `cpufreq_gov_big_max=`) |
| `ocp_guard=2` | and the five OCP registers read once on cpu 4, the flag read on each interrupt and each second. Nothing written |
| `ocp_guard=3` | stage one: armed as Samsung's driver runs it (IRP, step-down, release, watchdog); the ceiling still 1794 MHz |
| `ocp_guard=4` | stage two: the rule of (2)(b). Refuses today (2)(c) |
| `ocp_guard_flags=1` | also the PMIC's INT5 each second (reading clears it, as Samsung's PMIC interrupt thread does) |

On 16.5, `ocp_guard` is refused with `cpufreq_ocp=1` or `cpufreq_ocp_regs=1` (two drivers of one block).

The lines (on the console and, on 16.5, the loader's log):

- `cpufreq_gov: ocp_guard=N: SPI 213 (exynos-ocp's 181), rising edge, on the boot processor: attached (0); the M3s' power f f f f ...`
- `cpufreq_gov: ocp_guard: the S2MPS18's OCP_WARN1 (CPU) 0x.. (0), OCP_WARN2 (GPU) 0x.. (0): the CPU warning on|NOT on ...`
- `cpufreq_gov: ocp_guard: reading the OCP registers on cpu 4 now (...)` then `... read: OCPTHROTTCTL ..., GCUCTL ... (interrupt on|OFF), OCPINTCTL ... (IRP n), OCPTHROTTCNTA ..., OCPTIDEPBOTTHRESH ...`
- `cpufreq_gov: ocp_guard: SPI 213 raised by the GIC by software: the handler ran|did NOT run`
- `cpufreq_gov: ocp_guard: level N: ARMED as Samsung's driver runs it ...|NOT armed|observing only`
- every ten seconds: `cpufreq_gov: ocp_guard: S s: level N armed|not armed|DEAD | interrupts I (tests T) | throttles X, polled P, released R | clip K | IRP n | PMIC warning on | M3 power f f f f | buck2 limit B`
- `cpufreq_gov: ocp_guard: THROTTLE (interrupt, n so far): OCPTHROTTCTL ...; the M3s' ceiling A -> B kHz (rate C)`
- `cpufreq_gov: ocp_guard: DEAD for this boot: <why>; the M3s stay at or under 1794000 kHz`
- `cpufreq_gov: ocp_guard: stage two: <reason> (n M3s on, big m mC)`

## (4) The phone tests

In order; each needs the one before it to have passed. Every boot also has the usual `cpufreq_gov=1 tmu=3`. **Stop at once,
and go back to a boot without `ocp_guard`, on**: any reset or panic; a throttle line whose clip is not followed by the M3s at
or under it in the next summary line; a big-zone temperature above 83 C (`temp big` in the summaries); a `DEAD` line (the
test has failed: report the reason, do not repeat it with a higher level).

| # | Boot-arguments added | Lines to read | Pass | Unattended? |
|---|---|---|---|---|
| T1 | `ocp_guard=1` | the attach line, the OCP_WARN1 line, the self-test line, the ten-second lines for five minutes | attached (0); the handler ran; `tests` going up one a minute, `interrupts` equal to `tests`; the M3s at 1794 MHz at most; record OCP_WARN1's value (0x80 or not) and the PMU power nibbles | **yes**: no M3 register, no write but to the GIC's pending bit |
| T2 | `ocp_guard=1 ocp_guard_flags=1` | as T1, `buck2 limit` | as T1; `buck2 limit` stays 0 at 1794 MHz under the five-minute load of 17 November | yes |
| T3 | `ocp_guard=2` | the "reading ... now" line, then the "read:" line within the second; the summaries going on | the "read:" line arrives; GCUCTL's interrupt "on"; the governor's summaries go on afterwards; record the five values. **If the "read:" line never comes, the registers are not reachable from this kernel: the rest of the plan stops here** | **no**: the S9+ hung on this read under 16.5; the owner present to pull the battery/force a restart and to take the boot argument out |
| T4 | `ocp_guard=3`, idle five minutes | `level 3: ARMED`; ten-second lines | ARMED; `IRP 3` at 1794 MHz (and lower IRPs when the governor lowers the rate: 650 MHz 0, 1261 MHz 1); tests passing; no `DEAD` | owner present the first time (the first writes to the block), then yes |
| T5 | `ocp_guard=3`, the five-minute load of 17 November | ten-second lines, `temp big` | no throttle expected; if one comes: the THROTTLE line, the M3s under the clip in the next summary, `released` going up once the load falls | yes, after T4 |
| T6 | `ocp_guard=3`, an hour of normal use | the same | no DEAD; `interrupts` = `tests` + `throttles`; no reset | yes |
| T7 | `ocp_guard=4` | `stage two: ...` | says "more M3s on than Samsung allows ..." and the M3s stay at 1794 MHz: the rule refuses as it must | yes |
| T8 | (only once the fork can take an M3 offline and start it again, a separate piece of work with its own tests) `ocp_guard=4` | `stage two: ALLOWED: 2314000`; THROTTLE lines; `temp big` | the M3s at 2314 MHz only with two on; any throttle stepping them down and releasing; 2314 never with three or four on; the temperature under 83 C | **no**: the owner present, short runs first |

## (5) What is not built, and why

- **The PMIC write.** If S-Boot leaves OCP_WARN1 off, Samsung's kernel would turn it on; this one does not, and the guard does
  not arm. Writing it is a PMIC write the owner has not allowed (16.5's existing `cpufreq_ocp=1` does write it).
- **Any write to the block's thresholds or throttle settings.** Samsung's kernel makes none; their values are firmware's.
- **A trip provoked on purpose.** No safe way is in the source.
- **The offline path for the M3s** (2)(c): a separate piece of work in the fork, with the same unproven return through the
  reset vector as c2, and a reported failure on the S9+. Until it exists, stage two refuses, and that is the intended result.
- **A recovery from an undefined-instruction exception** around the register read: XNU panics on one in the kernel, which is
  at least visible. A read that traps to a higher level and never returns (the S9+'s case, perhaps) cannot be recovered from
  by anything in the kernel; T3 is therefore attended.

What it would take to run above 1794 MHz as safely as Samsung does: T3 passing (the registers reachable), T4-T6 passing, the
M3 offline path built and proven (CPU_OFF and CPU_ON again, many times), then T8. Short of all of that, the honest answer is
that the higher rates cannot be made as safe as Samsung's own kernel makes them on what is known today.

## Not verified

None of this has run on a phone. Built for iOS 16.5 (`starlte_defconfig`; `star2lte_defconfig` and `crownlte_defconfig`
compile) and iOS 27 (STARLTE; FRANKEL builds without it). The SPI number, its trigger and the register encodings are
Samsung's; that the S9's firmware leaves the block armed and the SPI in Group 1 under Scion as it does under Samsung's
kernel is assumed, and T1, T3 and T4 are what test it.
