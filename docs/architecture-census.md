<!-- SPDX-License-Identifier: MIT-0 -->
# The census: every large file, what is in it, and how it is split

The files over about 800 lines of `drivers/`, `socs/`, `userland/src/` and `tools/`, and the S9 and Starbite files of the
fork, read on 17 November 2026 (`main` at 131697c; the fork's `scion/xnu-12377` at 1532845), with the open pull requests
that touch each (`gh pr list --state open` in both repositories, that evening). The layers and names are those of
[architecture.md](architecture.md). Line ranges are of the file at those commits; a range named by its section is the
file's own `/* ---- ... ---- */` heading.

**Value** is how much easier the next bug or feature in that code becomes (3: a security boundary or a parser, or code
others build on; 1: a bring-up file few will touch again). **Safety** is how safely it can be split now (3: no open pull
request, host tests exist or the part is pure; 1: an open pull request touches it, so it waits). The order of work is by
safety first, then value.

## Open pull requests and the files they hold

| Repository | Pull request | Large files it touches |
|---|---|---|
| scion-kernel | #319 (modem: a file request counted against its own length) | `drivers/modem/hakios.shannon.c` |
| scion-kernel | #320 (video decoder and torch: stream sizes checked) | `drivers/media/mfc/hakios.exynos_mfc.c` |
| scion-kernel | #328 (iOS 27 camera) | `drivers/camera/exynos/hakios.exynos_camera.c`, `userland/src/camera_device.c` |
| scion-kernel | #329 (Anan Jaser's work of 12 to 16 November, 87 files) | `hakios.mali.c`, `hakios.mali.cpp`, `hakios.ssp.c`, `hakios.exynos_dpu.c`, `hakios.dwc3.c`, `hakios.exynos_mfc.c`, `hakios.stubs.c`, `tools/mesa/hakios-drm/hakios.drm.c`, `userland/src/inject.c`, `init.c`, `metalvk/NativeBridgeMetalFrontend.mm`, and new files over the ceiling (`gnssd.m` 1,936 lines, `metal_tri.m` 1,361, `gps_app.m` 1,073) |
| scion-kernel | #330 to #333 | documents and `tools/ios27/` scripts only |
| xnu | #29 (StarbiteMali: read-only pages, a buffer freed once) | `bsd/dev/starlte_gpu.c`, `iokit/Kernel/arm/StarbiteMali.cpp` |
| xnu | #34, #36 | `bsd/kern/kern_exit.c`, `osfmk/kern/exception.c`, `bsd/kern/decmpfs.c`: none of the files below |

## The kernel's drivers (`drivers/`, 44 files over 800 lines)

### Free now: no open pull request

| File | Lines | What is in it | Proposed split | Value | Safety |
|---|---:|---|---|:-:|:-:|
| `storage/ufs/hakios.ufshc.c` | 4044 | 1-325 head (the facts of four hosts), UFSHCI registers, UPIU and query constants; 326-653 the SoC hooks (`struct ufshc_soc`, `hc_socs[]`) and the driver's 56 file-scope variables; 654-812 the Exynos 9810's vendor registers; 813-862 the Tensor G5's; **863-1285 the whitelist** (`hc_protected`, `hc_refusal`) and the UPIU builders (`hc_make_*`), pure, with its in-kernel self-test; 1286-2101 one command in one slot (the transport); 2102-2240 own mode's control commands; 2241-2604 shared mode's proof and switches; 2605-2806 ufs=3's fences said into the loader's log; 2807-2961 shared mode's probe; 2962-4044 own mode (take-over, units, GPT windows) | `hakios.ufshc_upiu.h` (codec: constants, `be32`/`be64`, `hc_protected`, `hc_refusal`, the builders; host test `tools/ufshc-upiu-test.sh`); `hakios.ufshc_exynos.c` and `hakios.ufshc_lga.c` (SoC blocks, with `hakios.ufshc.lga.h` folded in); `hakios.ufshc_slot.c` (transport); `hakios.ufshc_shared.c`, `hakios.ufshc_own.c` (policy of each mode); `hakios.ufshc_diag.c` (self-test, fences report); `hakios.ufshc_internal.h` (the state as one struct, the locks) | 3 | 3 |
| `storage/ufs/hakios.ufs.c` | 3755 | 1-330 head, the loader's page ABI (`struct loader_block`), write fences, state, statistics sysctls; 321-387 the var-store handshake sysctls; 388-830 the loader's mailbox (requests, reads, writes); 831-991 the data volume; 992-1353 the block and character devices, the end of a run; 1354-1470 the check and bench (diagnostics); 1471-1922 own driver against the loader: units, native check, native bench; 1923-2470 own mode: never-written list, the GPT read and checked, the self-test; 2471-3480 ufs=3: summary, references, fences, soak, fallback; 3481-3755 bring-up | `hakios.ufs_gpt.h` (codec: the GPT header and entries checked, the never-written names); `hakios.ufs_loader.c` (transport); `hakios.ufs_os.c` (devices, sysctls); `hakios.ufs_own.c`; `hakios.ufs_switch.c` (ufs=3); `hakios.ufs_diag.c` (check, benches, soak, self-tests); `hakios.ufs.c` (policy and init) | 3 | 3 |
| `bluetooth/bcm4361/hakios.bcm4361_bt.c` | 2158 | 1-255 head, UART_BT, CMU and pin facts; 256-596 MMIO, pins, the UART (transport), the PCAL6524 expander (chip), power; 597-973 HCI commands, identify, the patch file (two `module_patch` per kernel); 974-1026 baud; 1027-1236 the scan (`ad_name`, `scan_event`: parsers); 1237-1277 console; 1278-1478 `/dev/cu.bluetooth` (tty); 1480-1748 `com.apple.uart.bluetooth` and `bt_trace` (`trace_line`, `trace_bytes`: parsers); 1749-1905 `bt_vsc_ok` (`vsc_byte`: a stream filter) and the control's pump; 1906-2024 publishing, the address (`addr_parse`); 2025-2158 the steps and thread | `hakios.bcm4361_bt_scan.h`, `_trace.h`, `_vsc.h` (codecs, replacing the cut-out test `tools/bcm-bt-parsers-test.sh`); `_uart.c` (transport and expander); `_hci.c` (commands and patch); `_os.c` (tty and kernel control); `_diag.c` (console, scan); `hakios.bcm4361_bt.c` (steps as a table) | 3 | 3 |
| `audio/abox/hakios.exynos_abox.c` | 2362 | 1-375 head, register map; 376-468 the amplifiers' hook; 469-815 steps power, clocks, pins, UAIF, route; 816-1162 bypass and the DMA area; 1163-1550 the stream (fill, start, run, sink); 1551-1772 recording; 1773-2096 headphones, putting back, the thread (already a step table); 2097-2245 init; 2246-2362 console | `_stream.c`, `_rec.c`, `_hp.c`, `_diag.c`; register facts to `socs/exynos9810/include/` | 2 | 3 |
| `power/exynos/hakios.exynos_cpufreq.c` | 1531 | 194-422 inputs: touch and frame boosts, their sysctls; 423-628 limits: idle CPUs, the big cluster, over-current (`ocp_*`); 629-817 the policy's predicates and boost; 818-925 the ACPM DVFS request and read-back; 926-1037 measuring, cores; 1038-1109 `decide`; 1110-1211 OCP poll, temperature, cooling; 1212-1297 statistics; 1298-1531 thread and start | first make the fork's copy (1339 lines, 656 of 808 long lines shared) the same file ([architecture.md](architecture.md), item 3); then `_decide.h` (codec: the decision as a function of its inputs, host-tested), `_ocp.c`, `_dvfs.c`, `_os.c`, `_diag.c` | 3 | 2 |
| `camera/exynos/hakios.exynos_camera_star2.c` | 4482 | the S9+'s copy of `hakios.exynos_camera.c` (1528 of its 2375 long lines are in it) with the rear, tele and autofocus parts | not split as it is: merged into one camera core with `hakios.exynos_camera.c` ([architecture.md](architecture.md), item 2), after #328 | 3 | 1 |
| `camera/exynos/hakios.exynos_camera_rear.c` | 1395 | 1-148 head; 149-196 the S2MPB03; 197-546 the bus, pins, clock; 547-1395 up, down, mode, and the S5K2L3's register tables | `hakios.s5k2l3_tables.h` (data); the rest a chip driver of the camera core | 2 | 3 |
| `camera/exynos/hakios.exynos_iris.c` | 1721 | sensor power (527-585), its own `isp_up`, `isp_down`, `link_setup` (829-1056, the front camera's again), capture, the fingerprint probe (1420-1483), the thread | joins the camera core with item 2; `fp_probe` to its own file | 2 | 2 |
| `camera/exynos/hakios.ak737x.c` | 901 | 344-455 the EEPROM and the chip; 457-501 the contrast measure (pure); 502-598 power; 599-901 the search | `hakios.ak737x_af.h` (codec: the measure and the search's arithmetic, host-tested) | 2 | 3 |
| `soc/exynos/hakios.exynos_tee.c` | 1272 | 393-677 MobiCore start and notifications (transport); 678-923 sessions; 924-1084 Keymaster's answers parsed (`km_parse`); 1085-1216 HMAC and the Keymaster test | `hakios.exynos_tee_km.h` (codec: `km_parse`, host-tested: it reads what the secure world sends); `_mc.c` (transport); `_diag.c` | 3 | 3 |
| `soc/exynos/hakios.exynos_acpm.c` | 881 | 200-357 setup and the request queue; 358-678 PMIC and DVFS requests; 679-881 the bus report thread | `_diag.c` (the report) | 1 | 3 |
| `soc/exynos/hakios.exynos_m3.c` | 907 | 212-426 what the PMU must hold before CPU_ON; 427-862 the coherency ping test | `_diag.c` (the test) | 1 | 3 |
| `audio/codec/hakios.cs47l93.c` | 1070 | 155-284 registers over SPI, reset and supplies; 285-750 the steps; 751-906 the headphones; 907-1070 console | `_diag.c` (console) | 1 | 3 |
| `audio/codec/hakios.max98512.c` | 906 | 313-495 the bus, 16-bit addresses; 496-684 the chips; 685-821 ABOX's hook; 822-906 steps | `_diag.c`; the bus helper shared with other 16-bit-address chips | 1 | 3 |
| `power/max77705/hakios.max77705.c` | 1125 | 1-351 the fuel gauge and charger; 352-576 the USB-C switch and VBUS; 577-1125 the haptic motor | three chip-function files: `hakios.max77705.c`, `_usbc.c`, `_haptic.c` | 2 | 3 |
| `storage/hakios.apfs_check.c` | 1872 | 1-840 the walk; 841-1001 getting there; 1002-1331 `apfs_check=7`, a write; 1332-1872 `apfs_check=8`, the store | by mode: `_walk.c`, `_write.c`, `_store.c` (all diagnostics) | 1 | 3 |
| `storage/ufs/hakios.ufshc.lga.h` | 817 | the Tensor G5's host from cold: CPM mailbox, power, the PHY, the link | a translation unit, `hakios.ufshc_lga.c` (a header of code today) | 1 | 3 |
| `display/panel/hakios.s6e3ha8_gamma.h` | 913 | a codec: the panel's gamma worked out from its calibration, with tables | none: one responsibility, data; listed | 1 | 3 |
| `input/touch/hakios.focaltech_fw.h` | 7288 | generated firmware data | none; listed | - | - |
| `wifi/brcmfmac/hakios.exynos_pcie.c` | 999 | 75-387 where, pins, the PHY, DBI; 388-630 the steps; 631-999 the radio driver's interface | `_phy.c` (SoC block), `_os.c` | 2 | 3 |
| `wifi/hakios.apple80211.c` | 833 | 89-231 the request layouts; 232-754 get, set, events; 755-833 dispatch | `hakios.apple80211_layout.h` (codec: what user space sends, host-tested) | 3 | 3 |
| `net/hakios.net.c` | 1485 | 1-336 en0 and en1; 337-445 DHCP and IPv4 seen in passing (parsers); 446-1003 ioctl, attach, output; 1004-1485 ping, DNS and TCP self-tests | `hakios.net_seen.h` (codec); `_diag.c` | 2 | 3 |
| `virtio/hakios.virtio.c` | 1032 | sections: virtio-pci, virtio-mmio, the split virtqueue, discovery, features, configuration, queues, interrupts | `_pci.c`, `_mmio.c`, `_vq.c` (the ring arithmetic as a codec) | 2 | 3 |
| `gpu/vgpu/hakios.vgpu3d.c` | 1173 | 81-236 freestanding helpers; 237-628 the control queue; 629-946 the stream and its encoding; 947-1173 self-test | `hakios.vgpu3d_wire.h` (codec: gfxstream encoding) | 1 | 3 |
| `gpu/vgpu/hakios.vgpu3d_xnu.c` | 1311 | SCGPU: the stream ring, ops, resources, the device, the self-test | `_ring.c`, `_os.c`, `_diag.c` | 1 | 3 |
| POCO: `modem/hakios.mpss.c` 6383, `wifi/hakios.wifi.c` 4432, `audio/hakios.adsp.c` 3295 | | each its own copy of Qualcomm's remote-processor stack: registers, SMEM, SMP2P, the mailbox, RPMh, TrustZone's PAS, GLINK, QRTR, the link's thread; then its own services (the modem's RMTFS, TFTP, pd-mapper, IPA handshake and network; the Wi-Fi's firmware exchange; the DSP's APR and battery manager) | `drivers/soc/qcom/` shared ([architecture.md](architecture.md), item 1); each driver keeps its services | 2 | 3 |
| POCO: `camera/hakios.camera.c` 4754, `gpu/adreno/hakios.adreno.c` 4114, `audio/hakios.mic.c` 1734, `sound.c` 1532, `speaker.c` 1237, `input/touch/hakios.goodix.c` 1562, `haptic/hakios.haptic.c` 1024, `gpu/hakios.gpu.cpp` 1412, `modem/hakios.qmi.c` 963, `ipa.c` 872 | | bring-up in numbered sections (the Adreno's 22 steps, the camera's CCI, PM8008, camcc, CSID/IFE); "I2C by hand" and the pin helpers repeated in camera, haptic, speaker and touch | the shared GENI/I2C and pin helpers to `drivers/soc/qcom/`; QMI's coding (`hakios.qmi.c` 197-588) as a codec; the steps as tables | 1 | 3 |

### Waiting for an open pull request

| File | Lines | What is in it | Proposed split | Waits for |
|---|---:|---|---|---|
| `modem/hakios.shannon.c` | 2890 | 179-582 addresses and helpers; 583-1101 stages 1 to 3 (look, load and check, start); 1102-1538 stage 4, the link (SIPC frames); 1539-1599 the data rings; 1600-2216 `cp_if=1`, `pdp_ip0`; 2217-2420 the CP's messages; 2421-2890 stage 6, the data context | `hakios.shannon_sipc.h` (codec: frames, file requests, the checks #319 adds, with #319's host test moved to include it); `_boot.c`; `_link.c` (transport); `_net.c` (OS); `hakios.shannon.c` (stages as a table) | #319 |
| `media/mfc/hakios.exynos_mfc.c` | 1336 | 250-400 SMC and the carve-out; 401-506 the command channel; 507-779 an instance: open, header, buffers, frame; 780-993 the block and its firmware; 994-1080 self-test; 1081-1277 `/dev` and its ioctls; 1278-1336 thread, init | `_os.c` (node, ioctls), `_fw.c`, `_diag.c`; #320's `hakios.mfc_check.h` is already the codec | #320, #329 |
| `camera/exynos/hakios.exynos_camera.c` | 3692 | 289-598 the S2MPB02, the bus, pins and clock; 599-832 the rear camera's supplies; 833-1149 ISPPRE, CSIS1, D-PHY, DMA, the sensor's registers; 1150-1316 stopping; 1317-2350 steps 3 to 5; 2351-3692 the run, `/dev/hakios-camera`, sessions, the thread | the camera core ([architecture.md](architecture.md), item 2) | #328 |
| `gpu/mali/hakios.mali.c` | 2216 (2395 with #329) | 187-414 log, CPU binding, MMIO; 415-499 reset, cores, a write job; 500-690 the page tables; 691-842 PLL, rails, SMC; 843-1347 nine steps; 1348-1712 the job interface HakiosMali uses; 1713-1972 the governor; 1973-2216 the worker, queue, thread, init | `hakios.mali_pt.h` (codec: descriptors), `_mmu.c`, `_g3d.c` (SoC block: PLL, rails, domain), `_jobs.c`, `_gov.c`, `hakios.mali.c` (steps) | #329, and in the fork #29 |
| `gpu/mali/hakios.mali.cpp` | 755 (1086 with #329) | the IOKit service and user client | `_uc.cpp` (the user client's selectors, with their argument checks) | #329 |
| `input/ssp/hakios.ssp.c` | 2594 (2831 with #329) | 234-472 MMIO, pins, CHUB power; 473-662 SSI frames (transport); 663-1012 packets, acks, reliable sends, RPCs; 1013-1160 the patch; 1161-1505 SSP, samples, calibration; 1506-1552 `parse_triple`; 1553-1779 `convert` (pure), `/dev/hakios-sensors`; 1780-1875 the self-read; 1876-2086 `/dev/hakios-gnss`; 2087-2233 steps; 2234-2393 console experiments; 2394-2594 init, console | `hakios.bcm_shub.h` gains `convert` and `parse_triple`; `_ssi.c` (transport), `_os.c` (both nodes), `_diag.c`, `hakios.ssp.c` (steps) | #329 |
| `usb/hakios.dwc3.c` | 2244 (2294 with #329) | 402-588 registers, TRBs, endpoint commands; 589-761 ep0 and control requests; 762-1005 pictures over USB; 1006-1196 the bulk pump and events; 1197-1709 the console (sysctl, devsh, swipe, backlight, dispatch); 1710-1890 the end of a run, DMA, core init; 1891-2244 bring-up, link, VBUS, init | `_ep.c` (transport), `_ep0.c` (with `hakios.usb_acm.h`), `_console.c` (OS and diagnostics: the console's command table), `_shot.c` (diagnostics), `hakios.dwc3.c` | #329 |
| `display/dpu/hakios.exynos_dpu.c` | 749 (1043 with #329) | DECON and DPP | `_decon.c`, `_dpp.c` once over; and the fork's diverged copy (item 3) | #329 |

## The fork's S9 and Starbite files (`Scion-Project/xnu`, `scion/xnu-12377`)

Apple's code, the vendored HFS Plus (`bsd/sweet_hfs/`), BearSSL and libpthread are not counted: they are third-party and
kept as they came.

| File | Lines | What is in it | Proposed split | Value | Open PR |
|---|---:|---|---|:-:|---|
| `bsd/dev/starlte_gpu.c` | 2279 | the Mali-G72 probe (stage 1), drawing tests, the present path (`starlte_gpu_present_*`), user mappings, the user queue (`sgu_*`: worker, retire) | `starlte_gpu_present.c`, `starlte_gpu_uq.c` (the queue StarbiteMali feeds), `starlte_gpu_diag.c`; GPU state as one struct shared with `hakios.mali.c` under one lock ([architecture.md](architecture.md), item 5) | 3 | #29 |
| `iokit/Kernel/arm/StarbiteMali.cpp` | 1057 | the IOService and its user client speaking panfrost's DRM interface | the selectors' argument checks as a codec (`starbite_mali_args.h`), host-tested | 3 | #29 |
| `iokit/Kernel/arm/SweetFramebuffer.cpp` | 2021 | IOMobileFramebuffer: the splash buffer, swaps and present, the user client's selectors, vsync | `SweetFramebufferUserClient.cpp`, `SweetFramebufferSwap.cpp` | 2 | none |
| `iokit/Kernel/arm/SweetSurface.cpp` | 1314 | IOSurface's user client; `sweet_surface_layout` (335-403: plane sizes and strides from what user space asks, pure) | `sweet_surface_layout.h` (codec, host-tested: it sizes memory from user input) | 3 | none |
| `bsd/dev/herolte_ufs.c` | 1918 | the Galaxy S7's UFS root device: SCSI, partitions, and `ufs_take_over_host` (656-1920), one function of 1,265 lines | the take-over as a step table of named functions; then the same split as `hakios.ufshc.c`, or this driver replaced by it with an `exynos8890` hook | 2 | none |
| `bsd/dev/starlte_sd.c` | 1775 | 1-1038 the DesignWare MSHC; 1039-1299 the MBR and `sdroot=` range (a parser); 1300-1543 the block device; 1544-1775 the probe | `starlte_sd_mbr.h` (codec), `starlte_sd_bdev.c`, `starlte_sd_diag.c` | 2 | none |
| `bsd/dev/herolte_gpu.c` | 1396 | the Mali-T880 probe and drawing; `hg_draw` (641-921) one function | `hg_draw` as steps | 1 | none |
| `bsd/dev/hakios.exynos_cpufreq.c` | 1339 | the fork's copy of the governor | deleted in favour of `drivers/power/exynos/` (item 3) | 3 | none |
| `osfmk/arm64/sweet_a64e.c` | 1284 | arm64e emulation by feature: DotProd, FHM, SHA3/SHA512, LSE and LRCPC (560-1284) | one file per feature group, the decoders as codecs; `tools/ios27/a64e/difftest/` already tests some on a host | 2 | none |
| `bsd/dev/starlte_touch.c` | 1179 | ACPM for the touch supply, the sec_ts protocol, events, the script thread | `starlte_touch_event.h` (codec: what the controller sends), `_diag.c` | 2 | none |
| `iokit/Kernel/arm/hid/SweetHID.cpp` | 1147 | HID event services for backboardd | by service | 1 | none |
| `osfmk/arm64/sweet_c2.c` | 830 | core power-down in idle, its decision and its checks | `c2_decide` as a codec | 1 | none |
| `bsd/dev/herolte_touch.c` | 816 | the S7's touch | none | 1 | none |

## `userland/src/` and `tools/` (50 files over 800 lines; reported, not yet enforced)

These are user space and the build's own tools. The same rules apply; they are gated once #329, which adds four more,
has merged. The ones that matter most for the kernel's quality are the parsers of what the kernel or a device sends:

| File | Lines | Note | Open PR |
|---|---:|---|---|
| `userland/src/inject.c` | 1840 | the injection library: hooks by framework in one file | #329 |
| `userland/src/init.c` | 1739 | init: mounts, the var store, daemons, by stage | #329 |
| `userland/src/var_store.c` | 1697 | the saved `/var`: the archive format (a codec, with `tools/var-store-read.py` its host twin) and the saving | none |
| `userland/src/camera_device.c` | 2003 | the camera's user-space device | #328 |
| `userland/src/camera_isp.c` | 1230 | the ISP's arithmetic in user space | none |
| `userland/src/inject_sb.c` | 3999 | SpringBoard hooks | none |
| `userland/src/eagl.c` 4781, `tools/ios27/ca-gpu/hakios/eagl.c` 4335 | | two copies of the EAGL layer | none |
| `userland/src/HakiosMetalCA.m` 8747, `hakiosmetal_air.m` 6342, `metal_drv_test.m` 7737, `HakiosMetal.m` 1534, `metalvk/NativeBridgeMetalFrontend.mm` 2084 | | the Metal plug-in and its tests | #329 (the last) |
| `tools/mesa/air/*.c` (`air_metal.c` 7072, `air_to_nir.c` 6753, `air_metal_test.c` 6765, `air_intrinsics.c` 2716, `air_run.c` 2685, `air_module.c` 2005, `air_print.c` 912, `air_metal.h` 1313) | | the AIR to NIR translator, kept with Mesa's layout | none |
| `tools/apfs/` (`apfs-tool.c` 1790, `apfs.py` 1205, `mkapfs.py` 938, `write-tests.py` 933) | | the APFS reader and writer used to check the kernel's | none |
| `tools/hakios-config.py` | 1239 | the configuration generator: reading conf files, defconfigs, the kernel's lists | none |
| the rest (`tools/air/*.py`, `tools/ios27/...`, `tools/qmi-test/test_qmi.c`, `tools/bcm_shub_test.c`, `userland/src/sbca.c`, `audio_plugin.c`, `gfx/gfx_gl.c`, `inject_media.c`, `triangle_app.m`, `vk_test.c`, `metal_stubdev.m`) | | `tools/arch-check.py --report` lists them | none |

## The order

1. **This page, [architecture.md](architecture.md) and its checks.**
2. **Codecs out of free files, each with a host test**: `hakios.ufshc_upiu.h` (the UFS whitelist: every command to the
   flash passes it, and no host test covered it); the Bluetooth parsers (`_scan.h`, `_trace.h`, `_vsc.h`, retiring the
   cut-out test); `hakios.exynos_tee_km.h`; `hakios.ufs_gpt.h`; `hakios.apple80211_layout.h`; `sweet_surface_layout.h` and
   `starlte_sd_mbr.h` in the fork.
3. **`profiles/ios-27.copy`'s build attribute** ([architecture.md](architecture.md), "A driver of several files").
4. **Splits into translation units of free drivers**: `hakios.ufshc.c`, `hakios.ufs.c`, `hakios.bcm4361_bt.c`,
   `hakios.max77705.c`, `hakios.exynos_abox.c`, the diagnostics out of `hakios.exynos_acpm.c`, `hakios.exynos_m3.c`,
   `hakios.cs47l93.c`, `hakios.apfs_check.c`.
5. **Structural work**: the governor made one file in both repositories, then split; the public headers moved here;
   Qualcomm's shared stack.
6. **The busy files**, in the order their pull requests merge: `hakios.shannon.c` (#319: smallest, and its codec makes
   #319's harness a plain include), `hakios.exynos_mfc.c` (#320, #329), then everything #329 touches (`hakios.ssp.c`
   first: the sensor hub's parsers are the next most exposed; then `hakios.mali.c` with the fork's `starlte_gpu.c` and
   `StarbiteMali.cpp` after xnu #29; then `hakios.dwc3.c`, `hakios.exynos_dpu.c`), and the camera core after #328.
