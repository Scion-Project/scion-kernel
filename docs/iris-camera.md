# The Galaxy S9's iris camera: where its frames go (17 November 2026)

The driver is `drivers/camera/exynos/hakios.exynos_iris.c` (`iris_s9=N`, off unless asked; its head has every register and
number). The sensor is a Samsung S5K5F1: infrared, 2400 x 2400 RAW8, the device tree's `fimc-is-secure@10`, on CSIS3, lit by the
S2MPB02's FLED2, which the sensor strobes. This page says why no frame of it has reached memory, and why none will by any means
this project accepts. **In short: Samsung's design confines the iris camera's frames to the secure world. The S9's own kernel has no
path that puts one in ordinary memory, and getting one there would mean going around the secure world's protection, which this project
does not do. The driver stays as it is (off by default), as a probe of the sensor and its power, and no more is built on it.**

Everything below about Samsung's kernel was read in `android_kernel_samsung_exynos9810` (branch `lineage-24.0`, commit `e5906667`,
GPL, read for facts only); "BSP" below means that tree. Line numbers are of that commit. Nothing was run for this page: there was no
phone. What the phones showed is the table in [What the phones showed](#what-the-phones-showed), from earlier runs.

## What the phones showed

From runs of 3 November (one phone, one kernel; the S9 device repository's `CAMERA.md` has the arguments):

| Run | Seen |
|---|---|
| `iris_s9=2` | the three LDOs on as expected; the sensor does not answer at 0x10 |
| `iris_s9=2 iris_s9_smc=1` | PREPARE (SMC 0x83000043) returns 0; the sensor answers: model ID 0x5061, version 0xA1, the OTP test 1 1 1; UNPREPARE returns 0 |
| `iris_s9=4 iris_s9_smc=1` | CSIS3 counts 6 frames, VC0's WDMA reports a frame end, ERR_CODE 0, and **0 of the buffer's 5,760,000 bytes change** |
| `iris_s9=5 iris_s9_smc=1`, longer exposure, more gain | CSIS3 counts 23 frames, no frame end in 600 ms, 0 bytes change |

The front camera's frames, through CSIS1, the same DMA common block and the same ISPPRE SysMMU, do land in the same physical buffer.
So the link works, the DMA engine runs, and the difference is something about CSIS3 after PREPARE.

## 1. PREPARE and UNPREPARE in Samsung's kernel

- The S9 is built with `CONFIG_SECURE_CAMERA_USE=y` (`arch/arm64/configs/exynos9810-starlte_defconfig`, line 399), the option of
  Samsung's vendor layer that is described only as "Use secure camera" (`drivers/media/platform/exynos/fimc-is2/vendor/mcd/Kconfig`,
  145-150).
- The numbers: `MC_SECURE_CAMERA_SYSREG_PROT` 0x82002132, `_INIT` 0x83000041, `_CFW_ENABLE` 0x83000042, `_PREPARE` 0x83000043,
  `_UNPREPARE` 0x83000044 (`fimc-is2/fimc-is-device-sensor.h`, 169-174). Only PREPARE and UNPREPARE are made on the S9.
- **When.** `fimc_is_sensor_s_input()` (`fimc-is2/fimc-is-device-sensor_v2.c`, from 1862) makes PREPARE, with no arguments, only when
  the scenario is the secure one, and before anything else of the sensor's (2102-2154): it records the state `FIMC_IS_SENSOR_SMC_PREPARE`
  and gives up on a nonzero answer. Then, in the normal world, the same function turns on the sensor's MCLK (2215), runs the module's
  power-on table (2222), initialises the CSI (2236) and the sensor over I2C (2246). UNPREPARE is made after the sensor is off: in
  `fimc_is_sensor_runtime_suspend()` (from 3515) the power-off table (3589) and MCLK off (3597) come first, then UNPREPARE (3605-3622).
  An error during `s_input` makes UNPREPARE at once (2264-2280).
- **SYSREG_PROT** is made only where `NOT_SEPERATED_SYSREG` is defined (sensor_v2.c 2103-2137, 3624-3639), which is the v5_2_0 IS chain's
  configuration (`ischain/fimc-is-v5_2_0/fimc-is-config.h`, 148), not the 9810's v6_0_0. The S9 does not make it.
- **What PREPARE changes is not in any source.** It is a call into EL3 and the secure OS; the BSP passes no address and gets back only
  a status. What the open code does show is which parts of the path the normal world keeps driving and which it keeps away from:
  - The normal world still drives, after PREPARE: the sensor's I2C (the `hsi2c@108D0000` Linux driver, through
    `fimc_is_sensor_write8`; stream on is a write of 1 to 0x0100, `fimc-is-device-module-5f1.c` 541-573), its regulators, its reset,
    `gpio_iris_en` and MCLK (the power table, 5f1.c 898-936), the CSI's set-up, and **the address the CSI's DMA writes to**
    (`csi_s_buf_addr()`, `fimc-is-device-csi_v4.c` 144-148, takes `frame->dvaddr_buffer[0]`). The IR LED's per-shot current, pulse
    width, delay and on-time are V4L2 controls that Samsung's normal-world camera service sets (`fimc-is-video-sensor.c` 696-720).
  - The secure camera's channel is named: `SECURE_CAMERA_CH` is `CSI_ID_D`, CSIS3 (`ischain/fimc-is-v6_0_0/fimc-is-config.h` 167). The
    unused emulation path (`SECURE_CAMERA_EMULATE`, commented out at 166) shows how that firmware is parametrised: it is told the CSI
    channel and the ION heap of the secure camera, and a memory range (`fimc-is-core.c` 1294-1303, config.h 168-169).
  - With `CONFIG_SECURE_CAMERA_USE`, the 9810's IS chain leaves CSIS3 alone in its shared helpers: the Q-channel helpers do not map or
    write CSIS3's control register (0x16260004; `ischain/fimc-is-v6_0_0/fimc-is-hw-chain.c` 905-947), and the DMA input mux maps
    SYSREG_ISPPRE only up to 0x424 and never writes DMA 3's mux, `SC_CON8` at 0x424 (hw-chain.c 591-595, 670-673).
  - The ISPPRE power domain is under TrustZone protection: its `need_smc` in the device tree is 0x16210204 (the DTS, 3825-3829), the
    address that `exynos_pd_tz_restore()` and `_save()` pass to SMC 0x82000410 after power-up and before power-down
    (`drivers/soc/samsung/cal-if/pmucal_local.c` 42-48, `drivers/soc/samsung/exynos-el3_mon.c` 327-337).

## 2. Why the sensor does not answer without PREPARE

The BSP does not say, and nothing in it is a pin, supply, clock or bus that the secure world takes: everything the sensor needs is
switched by the normal world, after PREPARE (above). The 5F1 has **one** power table, for the secure scenario (`SENSOR_SCENARIO_SECURE`,
defined in the file itself as 6, 5f1.c 58 and 898-936; no `SENSOR_SCENARIO_NORMAL` entries), so Samsung's kernel never powers this
sensor without having made PREPARE first.

On the phone, the iris driver's pin test found that iris_en (gpg0-7) and the bus pins (gpp4-6/7) read back the same before and after
PREPARE, while the bus works only after it; the S9 repository's `CAMERA.md` already concluded that this read-back tests nothing. So the
gate is somewhere this side cannot see: in EL3 or the secure OS, or a protection controller they set. The design is plain from the
order Samsung's kernel keeps: the sensor is reachable only between PREPARE and UNPREPARE, and in that window its frames go where
section 3 says. That pairing is the point of the secure scenario.

## 3. Where CSIS3's DMA writes after PREPARE

In Samsung's design the DMA's destination is a **secure buffer reached through the secure side of the ISPPRE SysMMU**:

- The device tree reserves `secure_camera`: 30 MB at 0xE1900000, `ion,untouchable` (DTS 81-86). An untouchable heap is a secure heap
  (`drivers/staging/android/ion/exynos/exynos_ion.c` 528-538): every buffer from it must be `ION_FLAG_PROTECTED`, otherwise the
  allocation fails (`ion_carveout_heap.c` 126-143), and a protected buffer cannot be mapped by a CPU (`ion.c` 1296).
- A protected buffer gets a **secure IOVA** from a pool covering 0x80000000 to 0xE0000000 (exynos_ion.c 35-36, 146-187) and is handed
  to the secure world with SMC `SMC_DRM_PPMP_PROT` (exynos_ion.c 210-247, the call at 233).
- The camera driver maps its buffers with `ion_iovmm_map()` (`fimc-is2/fimc-is-mem.c` 188 and 416). For a protected buffer that function
  returns the secure IOVA and maps nothing in the normal world's tables (`ion.c` 2408-2432). That number is what `csi_s_buf_addr()` writes
  into CSIS3's WDMA.
- The ISPPRE SysMMU (`sysmmu@16310000`, the device tree's `sysmmu_isppre`; DTS 8695-8706 and 14469) has a second, secure register frame,
  `sysmmu,secure_base` 0x16300000, and a secure fault interrupt. The normal world reads that frame only through SMC
  `SMC_DRM_SEC_SMMU_INFO`, one word at a time, to print a fault (`drivers/iommu/exynos-iommu-reg.h` 17-27, 277-347, 605-630;
  `exynos-iommu.c` 153-170, 225-252, 463-467).

What follows is inference, consistent with every observation and not proved: after PREPARE, CSIS3's DMA writes are secure transactions
and are translated by the secure context of the ISPPRE SysMMU, whose page table only the secure world fills. The iris driver writes the
physical address 0xD8000000 into the WDMA. To that context it is an IOVA, and one inside the secure IOVA window. Either it is not mapped,
so the writes fault (to the secure interrupt, which this side does not see) and are dropped, or it is mapped to secure memory. Either way
nothing lands in the normal buffer, while CSIS3 and the WDMA count frames with no error of their own, exactly as the phone showed. The
front camera, whose CSIS1 is not the secure channel, writes the same buffer normally.

The only normal-world way to see the secure fault is `SMC_DRM_SEC_SMMU_INFO`, the read-only diagnostic call Samsung's kernel makes. It
would only confirm the inference, and it is not added here.

## 4. Is there a normal-world capture path in Samsung's kernel?

No:

- The 5F1 is position 3, `SENSOR_POSITION_SECURE` (`fimc-is2/include/exynos-fimc-is-sensor.h` 53; the node's `position = <3>`), and the
  device tree's `secure_sensor_id` is 306, `SENSOR_NAME_S5K5F1` (DTS 11152; sensor.h 135). The module has only the secure scenario's
  power table (5f1.c 898-936); with any other scenario nothing would power it.
- The factory build changes only VDDD's voltage (`CONFIG_SEC_FACTORY`, 5f1.c 908-914). The factory's sysfs files for the iris camera
  report the module's version and OTP data read during init (`vendor/mcd/fimc-is-sysfs.c` 2661-2900); none returns a picture.
- There is no other sensor mode: one configuration, 2400 x 2400 RAW8 on VC0 (5f1.c 134-141).
- `USE_NONE_SECURED_CAMERA`, which would skip PREPARE, is defined nowhere in the tree; the emulation path is commented out.

## Verdict

**(ii) The iris camera's frames are confined to the secure world by design.** No configuration that Samsung's own kernel uses delivers
them to ordinary memory. The owner and Claudius set the limit: no bypassing, exploiting, patching or tricking the secure world (EL3,
Kinibi, TZPC, the secure SysMMU, Knox), and no reverse engineering of Samsung's trusted applications to defeat their protection. Every
remaining way to a frame in normal memory runs around that protection, so none is designed here. That includes the ordering that the
S9 repository's `CAMERA.md` listed as a next step (PREPARE, power up, UNPREPARE, then stream): it would use the secure world's own calls
out of their order to get frames out of the protected path, which is tricking it, and it should not be run. `iris_s9_smc=2` and `=3`
(holding ISPPRE across PREPARE; SYSREG_PROT, which the S9's configuration never calls) change nothing of the above and need not be run either.

The driver is not changed. What it does today stays useful and safe: with `iris_s9_smc=1` it proves the sensor, its supplies and
its OTP, and its eye-safety limits (the LED never lit by the driver, only the GPL kernel's LED values, captures of at most
`iris_s9_ms`, 2 s apart, 40 a run, the sensor stopped on every way out) are untouched.

## The one legitimate alternative, and what it would need

The secure world does have a use for these frames: Samsung's own iris trusted application, which receives them inside the secure OS and
answers with a result. Using it as it is meant to be used, through its normal interface, would need all of the following. **Nothing of
it is built, and each item is the owner's decision:**

1. **The secure OS running under XNU.** `drivers/soc/exynos/hakios.exynos_tee.c` (`tee_probe=N`) starts Kinibi and can load a trusted
   application from the phone's VENDOR partition (`tee_probe=3`, built, not run). A frame path needs more: the sessions, the shared
   memory and the notifications of a full client.
2. **Samsung's iris trusted application and its client protocol.** The binary would be loaded at run time from the owner's own phone,
   never copied into a repository. Its commands are not documented: learning them means reading Samsung's closed normal-world iris
   libraries for interoperability. That is the owner's decision, and it stops short of anything that changes or defeats what the
   application protects.
3. **Secure buffers made the way ION makes them**: an allocation in the `secure_camera` region given to the secure world with
   `SMC_DRM_PPMP_PROT`, and CSIS3's WDMA pointed at the secure IOVA it returns.
4. **Enrolment storage.** The application keeps its templates in the secure OS's storage. On the S9 that storage is reached through
   Android's storage proxy and may involve RPMB or EFS, which this project never writes (rule 4 of the agents' rules). Whether and where
   templates may be stored is the owner's decision; without it there is no enrolment.
5. **The IR LED's per-shot settings.** Samsung's camera service sets the LED's current, pulse width, delay and on-time per shot
   (fimc-is-video-sensor.c 696-720). Those values are not open, and the iris driver deliberately uses only the kernel's own. Any change
   there is an eye-safety question to settle before anything is written.
6. **What the result is worth.** The application would answer "match" or "no match" to XNU. With no Secure Enclave and no hardware key
   on the iOS side, that answer can only open the screen, like any convenience unlock ([BIOMETRICS.md](ios27/BIOMETRICS.md), option b).
   What it would add over Scion's own matcher is that templates and frames never leave the secure world, and Samsung's liveness checks.
   Whether the application works at all with an unlocked boot loader and a tripped Knox state is not known.

## An aside: the ISPPRE DTZPC address

The front-camera driver and this one pass 0x16210004 to SMC 0x82000410 as the ISPPRE domain's `need_smc`
(`EXYNOS_CAM_DTZPC_ISPPRE` and `EXYNOS_IRIS_DTZPC_ISPPRE` in `socs/exynos9810/include/hakios.exynos9810_soc.h`). The BSP's device tree
gives 0x16210204 for `pd-isppre` (DTS 3829), and every `need_smc` in it ends in 0x204. Which one the phone's own device tree holds was
not checked here (that would mean reading the phone's boot image). Not changed.

## Sources

- BSP (`lineage-24.0`, `e5906667`): `fimc-is2/fimc-is-device-sensor_v2.c`, `fimc-is2/fimc-is-device-sensor.h`,
  `fimc-is2/sensor/module_framework/modules/fimc-is-device-module-5f1.c`, `fimc-is2/fimc-is-device-csi_v4.c`, `fimc-is2/fimc-is-mem.c`,
  `fimc-is2/fimc-is-core.c`, `fimc-is2/fimc-is-video-sensor.c`, `fimc-is2/ischain/fimc-is-v6_0_0/fimc-is-config.h` and
  `fimc-is-hw-chain.c`, `fimc-is2/vendor/mcd/Kconfig` and `fimc-is-sysfs.c`, `drivers/staging/android/ion/ion.c`,
  `ion_carveout_heap.c` and `exynos/exynos_ion.c`, `drivers/iommu/exynos-iommu.c` and `exynos-iommu-reg.h`,
  `drivers/soc/samsung/exynos-el3_mon.c` and `cal-if/pmucal_local.c`, `arch/arm64/configs/exynos9810-starlte_defconfig`,
  `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts` (all paths of the media driver under
  `drivers/media/platform/exynos/`).
- This repository: `drivers/camera/exynos/hakios.exynos_iris.c`, `drivers/soc/exynos/hakios.exynos_tee.c`.
