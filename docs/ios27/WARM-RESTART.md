# iOS 27 on the Galaxy S9: restarting without a reset (the warm restart) (18 November 2026; built, not run on a phone)

Every restart of the S9 today goes through Samsung's signed boot loader, S-Boot: the PMU's software reset (or the
watchdog, or the keys) resets the SoC, S-Boot starts, vibrates the motor, shows its logo and after about ten seconds
starts BOOT again. S-Boot cannot be changed (no boot loader writes, no secure-boot bypass), and the motor woke the
owner's house on 17 November, which ended night testing. A **warm restart** does not reset the SoC at all: the running
XNU stops its devices and processors, turns its MMU off and starts the loader again, which starts the next kernel as on
a cold boot. S-Boot is not bypassed or patched; it is simply not in the path, because nothing resets.

This page is the design, what is built (behind `sweetwarm=`, off by default), and the plan for the phone, in steps.
**No phone has run any of it.**

Line numbers are those of the fork `Scion-Project/xnu` at `scion/xnu-12377` commit `27a3e12` and of this repository at
`main` commit `a521a09`, before this change, unless a file is said to be new.

## Contents

- The verdict
- How the phone boots and restarts today
- The design: the kernel's side, the contract, the loader's side
- Memory: where everything is during the handoff
- Caches, processors, exception levels
- The devices
- Safety: the point of no return, the watchdog, nothing written
- Recovery: iOS to TWRP, TWRP to iOS
- What of scion-boot-exynos9810 is used
- Tests without a phone, and QEMU
- The plan for the phone: step 1 and after
- Risks
- Not verified

## The verdict

Feasible, in steps. The parts that need no new facts about the phone are built: the kernel reads BOOT (read only) and
re-stages it, quiesces, and enters a second entry of the loader through an identity map with the MMU off; the loader
checks a re-entry record and the payload's SHA-256 and starts XNU through the same `xnu_enter` a cold boot uses. The
hard points, each with a step of its own below:

1. **The other processors.** XNU starts the S9's secondaries with PSCI `CPU_ON` (`iokit/Kernel/arm/SweetCPU.cpp`
   398-447) and panics if `CPU_ON` does not return 0 (443-446), so the next kernel can only start cores that are *off*,
   not parked in a loop. PSCI `CPU_OFF` (0x84000002, the vendor tree's `cpu_off`) is how Samsung's own kernel takes cores
   off on this firmware (CPU hotplug, `CONFIG_EXYNOS_HOTPLUG_GOVERNOR=y`, `arch/arm64/kernel/psci.c` 70-80:
   `cpu_psci_cpu_die` = `psci_ops.cpu_off(0x10000)`), so a clean off + on is very likely; this kernel has never done it
   (XNU's own `SweetCPU::haltCPU` panics, 454, which is why an M3 could not be taken offline: that path is not used
   here). Step 1 avoids the question with `cpus=1`; step 2 is `sweetwarm_smp=1`.
2. **Devices that write memory on their own** after the kernel lets go: the USB controller, the UFS host, Wi-Fi's PCIe,
   the audio box, the modem, the cameras, the GPU, the display. Each has a stop already used by the console's `return`
   except the GPU (a new stop: the driver's own soft reset) and the display (kept scanning the boot framebuffer, which
   stays valid).
3. **Caches across clusters** after a non-coherent copy (MMU off): handled by cleaning and invalidating by address the
   ranges the stub and the loader write (broadcast to every cluster) and by set/way on the boot core.
4. **What the next kernel finds** that S-Boot would not have left: a UFS host already taken over, DECON in the state XNU
   left it, a GIC with everything disabled, the sensor hub's firmware running. Each is a phone question (below).

TWRP *into* iOS without a reset needs kexec in TWRP's Linux kernel; Samsung's `exynos9810-starlte_defconfig` does not set
`CONFIG_KEXEC` (it is off by default), so a TWRP built from Samsung's kernel has none, and a TWRP kernel cannot be
rebuilt for this. That direction stays a (vibrating) reset. iOS into TWRP is designed and built as step 3.

## How the phone boots and restarts today

- **Boot.** S-Boot loads BOOT, an Android v0 boot image (`tools/ios27/s9/make-boot-image.sh`;
  `tools/ios27/starlte-pack-boot.py`: page 2048), and enters its kernel slot at EL1 with the MMU off: the slot is the
  loader Image (`xnuloader/sboot`, `xnu_boot.S`) with the SWEETXNU payload (kernel, Apple device tree, command line;
  `tools/ios27/herolte/mkpayload.py`) appended at the Image's 4 KiB-padded end (`build.sh`'s check, 57-61). The loader
  (`xnu_boot.c` `star_prepare_xnu`, 1558-1640) has a Bixby window, a splash, a POST, the catalog from UFS, fastboot and
  the menu, then `boot_xnu` → `xnu_enter` (1157-1223): XNU's segments, ADT and `boot_args` into its window (0x92000000
  or the second bank, 0x880000000), and `xnu_boot.S` turns the loader's MMU off and enters XNU with x0 = `boot_args`
  (66-101). One CPU; XNU starts the others itself (above).
- **XNU's RAM covers the loader.** `tools/ios27/adt-extra-ram.py` gives XNU seven extra ranges of the first bank,
  among them 0x80200000-0x8F000000 (RANGES, 33-40): the loader Image, its payload, S-Boot's FDT (0x8a000000) and initrd
  (0x89000000) are reused as XNU's RAM once it runs. **Nothing of the loader survives in RAM**: a warm restart has to
  bring it back. Left out of XNU's RAM, and so usable for the handoff: 0x8F000000-0x91FFFFFF (SMP trampoline and
  mailbox 0x8fffe000/0x8ffff000, GPU scratch 0x8fe00000, zswap_debug 0x91300000, S-Boot's log areas
  0x91202000-0x9179c000, the boot record 0x91ffe000 and the log page 0x91fff000; 0x91000000-0x91200000 is used by
  nothing but the loader's fastboot USB DMA below 0x91050000, `fastboot_dev.c` 6-10).
- **Restart from iOS** (`sweethalt=1`, `bsd/dev/herolte_ufs.c` 1160-1243, proven on the phone 17 November): `reboot(2)`
  syncs and unmounts, `PEHaltRestart(kPERestartCPU)` → `starlte_halt_restart` → `starlte_reset_normal` (1176-1188):
  PMU INFORM2/INFORM3 = 0x12345678/0x12345670 and SWRESET (PMU + 0x400). S-Boot, the motor, BOOT.
- **`loader_return()`** (`bsd/dev/starlte_usbcon.c` 90-103; the heat stop's fallback, `hakios.exynos_tmu.c` 269 and 374;
  the console's `return`) does **not** go back into the loader on this tree: it stops the amplifiers, the codec and
  ABOX, then `starlte_return_to_recovery` (`herolte_ufs.c` 1563-1574): the dump file, the log flushed,
  `starlte_reboot_recovery` (1542-1561: front camera and modem quiesced) and `starlte_reset_to_recovery` (1084-1099):
  INFORM3 = 0x12345674 and SWRESET, i.e. a reset into TWRP through S-Boot. (Scion's 16.5 line on the Qualcomm phones has
  a real return into its UEFI loader through a TTBR0 identity map, `osfmk/arm64/loader_return.c` of that tree; the
  identity map here is built the same way.)
- **Watchdogs.** XNU's `sweetwdog=` (`herolte_ufs.c` 1340-1481) is a timer-call watchdog that resets through the PMU; it
  does not drive the SoC's watchdog (1352-1353). The cluster-1 watchdog at 0x10060000 is armed by the UEFI path's
  loader (`xnuloader/wdt_exynos.c`, the facts in its head: prescaler 255, divider 128, 82 s at most, the PMU's
  `MASK_WDT_RESET_REQUEST` 0x40C and `DISABLE_WDT` 0x408 bit 23; a watchdog reset loses INFORM2/3 and S-Boot starts BOOT).

## The design

### The kernel's side (`bsd/dev/starlte_warm.c`, new)

1. **Prepare (can be undone).** On the boot processor (bound to it): refuse unless one processor runs or
   `sweetwarm_smp=1`; refuse with `fb_flip=`, `fb_dpp=` or `sweetfb_dpp=` (the DECON would not be on the boot framebuffer); check that TTBR0
   can map the stub (4 KiB granule, T0SZ 25-33). Read BOOT's header and kernel slot through the UFS driver's
   `hakios_ufs_read_partition` (read only; BOOT and RECOVERY added to its two names) into wired memory; check the slot:
   an arm64 Image whose bytes at +0x44 are `SCNR` and version 1 (a loader with the warm entry), the sizes (below), the
   SWEETXNU payload after it as the loader's `payload_bad` would check it, and the next kernel's span (from
   `phys_base`, `kernel_end - virt_base`, the ADT and `boot_args`). SHA-256 of the payload, from where it starts to the
   end of the slot (libkern's). Build the copy
   list: every staged 4 KiB page's physical address and its destination; a staged page that lies where something is
   copied to is replaced by one that does not (so the copy never reads what it already overwrote). Any failure: one line,
   the memory freed, the system runs on. `warm dry` stops here.
   **The sizes** (`warm_slot()`; `tools/warm_slot.c` is the same function for the host test). S-Boot loads the BOOT
   header's `kernel_size` bytes of the kernel slot to 0x80080000. The arm64 Image header's `image_size` is only how much
   room it keeps there, and `tools/ios27/starlte-pack-boot.py` pads it on purpose: the larger of the slot + 64 KiB and
   the template's (the TWRP kernel's 0x2842000, about 42 MB), so it is always larger than the slot. What is read
   again, hashed, copied (whole pages: the rest of the last page is the staging buffer's zeroes) and named in the
   record's `image_size` is therefore `kernel_size`; it must fit below 0x84000000. The header's `image_size` decides
   nothing here: it is only checked to be at least the slot and to end below the loader's `RAM_BASE` (0x92000000),
   which is what the loader's `boot_payload` needs of it on a cold boot too (it looks for the payload in
   `[_image_end, base + image_size)`). The first build checked `image_size <= round_page(kernel_size)` instead and so
   refused every image the packer makes (the phone's `warm dry`, 18 Nov: "the loader's image_size does not fit").
2. **Write the handoff area** (0x91060000, mapped at boot as device memory): the record, the stub, an L1 and an L2 table
   that map the 2 MiB around the stub to itself (read-only, executable, like 16.5's `loader_return.c`), the copy list.
3. **Point of no return.** The log flushed to UFS; the drivers stopped (below), the USB console last but the storage;
   the UFS driver's lock taken for good and the device's cache synced; the cluster-1 watchdog armed for 60 s; with
   `sweetwarm_smp=1` the other processors told to switch themselves off; interrupts off, both timers off, the GIC
   emptied (`pe_fiq.c` `a5xelte_gic_quiesce`, new: every line disabled, nothing pending or active, this core's CPU
   interface off); the staged pages cleaned and invalidated by address, then this core's whole data cache by set/way.
4. **The jump.** TCR's EPD0 cleared, TTBR0 = the identity L1, TLBs and the I-cache invalidated, branch to the stub's
   physical address. The stub (position independent, x0-x17 only): MMU, D-cache and I-cache off; the data cache cleaned
   and invalidated by set/way again (what the kernel wrote after its flush, its stack, goes out now; with the cache off
   nothing comes back in); the copy; the destination and the next kernel's span cleaned and invalidated by address
   (no stale line of them may stay anywhere, a cluster's L3 included); the I-cache; then the loader at 0x80080000 +
   0x40 with x0 = the record.

Ways in: the USB console's `warm` (`drivers/usb/hakios.dwc3.c`, new command: `warm`, `warm ARGS`, `warm line L`,
`warm recovery`, `warm dry`), `sysctl kern.sweetwarm_go=1|2` and `kern.sweetwarm_dry=1`, and with `sweetwarm=2` every
restart iOS asks for (`starlte_halt_restart`'s `kPERestartCPU` calls `starlte_warm_from_restart` first; if the warm
restart cannot be had it returns and the PMU reset happens as before). The reboot path has already synced and unmounted
the file systems by then (the 17 November proof): the warm restart reuses it as it is. Each warm boot's line gets
`sweetwarmed=N`; the next kernel says `STARLTE WARM: this boot was started warm (sweetwarmed=N)`.

### The contract (`xnuloader/sboot/scion_reentry.h`, new; one contract with scion-boot-exynos9810)

- **Entry:** the loader Image's base + 0x40 (a branch right after the 64-byte arm64 header; the cold entry stays at
  +0). At +0x44 "SCNR", at +0x48 the record's version: the kernel does not let go without them.
- **State:** EL1; MMU, D-cache and I-cache off; everything the loader reads cleaned to the point of coherency; DAIF
  masked; one CPU running (the boot CPU, MPIDR 0x81000000), the others PSCI-off; x0 = the record's physical address,
  x1-x3 = 0; the GIC empty and the timers off; the cluster-1 watchdog running when the record says so.
- **Record** (`struct scion_reentry`, 728 bytes, little-endian): "SCNR", version 1, size, reason, flags (bit 0 direct
  start, bit 1 the record's line is used and replaces the payload's, bit 6 with bit 1 it is appended instead, bit 2
  the menu, bit 3 re-staged, bit 4 watchdog armed, bit 5 keep it armed), entry index, CRC-32 (IEEE, over all 728
  bytes with the field 0), the line (608 bytes, XNU's `boot_args.CommandLine`, NUL-terminated) at 24, method at 632
  (0 = BOOT's XNU payload, 1 = an Android kernel + ramdisk), the watchdog's time at 636, the Image's address and size,
  the Android kernel's and ramdisk's, S-Boot's saved FDT at 688 (the FDT itself: 0x91101000, after the save's 4 KiB
  header "SCNRFDT1" + size at 0x91100000), and the SHA-256 of what is appended to the Image at 696. Agreed with the agent building
  scion-boot-exynos9810's second stage (its `include/boot.h`); either side changes it only with the other.
- **What the loader may not trust:** anything in RAM but the record (copied first) and what the SHA-256 covers. The
  Image's own bytes are not covered (the loader runs in them and writes its data and stack there before it can look); a
  corrupt loader most likely faults, and the watchdog resets.

### The loader's side (`xnuloader/sboot`)

`xnu_boot.S` `warm_entry` (new): the log page and the stack as the cold entry sets them, then `star_warm` (`warm.c`,
new): the record copied and checked (`warm_record.c`, new: magic, version, size, CRC-32, the line terminated, a direct
start, the Image where the loader runs), the MMU on as on a cold boot, the screen ("WARM RESTART"), the payload's
SHA-256, then `star_warm_start` (`xnu_boot.c`, new, 20 lines): BOOT's payload through the same `boot_info` and
`xnu_enter` the cold path uses, with the payload's line, the record's in its place, or the payload's with the record's
appended; method 1 through `ram_start`'s Android path (the loader's fastboot code) with the saved FDT. The watchdog is
stopped (WTCON 0, the PMU's mask bit set again) right before the OS. The handoff is the cold path's own (`handoff:` in
`xnu_boot.S`). No Bixby window, no splash, no POST, no catalog, no storage. A record or payload that is not right: the
reason on the screen, 10 s, the PMU's restart into recovery.

On a cold boot nothing changes unless the payload's line has `sweetwarm=`; then the loader also keeps a copy of S-Boot's
FDT at 0x91100000 (for step 3).

## Memory: where everything is during the handoff

| What | Where | Why there |
|---|---|---|
| The loader Image + payload (BOOT's kernel slot) | 0x80080000, up to 57.5 MiB (BOOT is 14080 blocks of 4 KiB) | where S-Boot puts it: the loader runs as after S-Boot |
| A TWRP kernel / ramdisk (step 3) | 0x84000000 / 0x88000000, below the loader's STAGE 0x8c000000 | `ram_start` copies them on to 0x92080000 / 0xa8000000 |
| The handoff area: record, stub, identity tables, copy list | 0x91060000-0x910fffff | in no OS's RAM, unused by the loader on this path |
| S-Boot's FDT, kept by the cold boot (step 3) | 0x91100000-0x911fffff | the same |
| The staged pages (sources of the copy) | anywhere in XNU's RAM but the destinations | replaced when they lie in one |
| The next kernel's span | `phys_base` (0x92000000 or 0x880000000) + kernel + ADT + `boot_args` | written by the loader |
| The boot framebuffer | 0xcc000000 | not in XNU's RAM; DECON keeps scanning it |

## Caches, processors, exception levels

- **Exception levels.** S-Boot leaves the loader at EL1 (`boot_env_bad`, `xnu_boot.c` 1350-1357, insists on it); XNU
  runs at EL1; EL2 and EL3 are Samsung's (the RKP region 0xaf800000 is in no OS's RAM); PSCI is SMC from EL1 to EL3.
  Nothing here changes EL2 or EL3; the warm restart is EL1 to EL1.
- **The boot core** (A55 core 0): after the copy with the cache off, its caches hold nothing of the destinations.
- **Other clusters.** A non-coherent write (the stub's, MMU off; the loader's, through its non-cacheable mapping of the
  XNU windows) does not snoop. A line of the same address left in another cluster's cache (an M3 cluster L3 that stays
  powered when its cores are off) could later be read by an M3 of the next kernel, or written back over the new data.
  The stub therefore cleans and invalidates by address (broadcast through the interconnect) the destination and the next
  kernel's span after the copy; a secondary cleans its own caches before `CPU_OFF` (step 2).
- **Secondaries.** Step 1: `cpus=1` (XNU's own boot-arg, `osfmk/arm64/machine_routines.c` 1176): there are none. Step 2:
  a cross-call to every other processor: interrupts off, timers off, its data cache cleaned and invalidated, then PSCI
  `CPU_OFF`; the next kernel's `CPU_ON` starts them as after S-Boot. If one does not answer in 20 ms: a PMU reset. If
  `CPU_ON` failed in the next kernel it would panic (`SweetCPU.cpp` 443-446) and the panic hook (`sweetreboot`) or the
  keys end it: no worse than a failed cold boot. The fallback, if this firmware's `CPU_OFF` turns out not to give a core
  back to `CPU_ON` (Samsung's hotplug says it does): keep the secondaries parked in a WFE loop in the handoff area and
  teach the next kernel to release them through a mailbox instead of `CPU_ON` (a change to `SweetCPU::startCPU` behind a
  boot-arg; designed only).

## The devices

| Device | Writes memory / drives power on its own? | At the handoff |
|---|---|---|
| USB (DWC3, the console) | yes: TRBs and event buffer | `hakios_usb_gadget_quiesce` (pull-up off, events masked, PHY off); the next kernel's console enumerates again |
| UFS host | yes (DMA, the driver's bounce buffer) | the driver's lock held for good after a cache sync; no command in flight. The next kernel takes the host over (`ufs_take_over_host`) from the state XNU left, not S-Boot's: a phone question |
| Wi-Fi (PCIe, BCM4361) | yes (DMA rings) | `hakios_exynos_pcie_quiesce` (chip in reset, unpowered), as `loader_return`'s path does |
| ABOX, amplifiers, codec | ABOX DMA | `hakios_max98512_quiesce`, `hakios_cs47l93_quiesce`, `hakios_exynos_abox_quiesce` (the console's `return` order) |
| Cameras | yes (when streaming) | `hakios_exynos_camera_quiesce` |
| Modem (Shannon) | shared memory | `hakios_shannon_quiesce` (back in reset, if `cp=` started it) |
| GPU (Mali) | yes, through its own MMU and page tables in XNU's RAM | `starlte_gpu_quiesce` (new, `starlte_gpu.c`): the driver's own soft reset (every job and the MMU's state gone) and its cores off, with interrupts off after the point of no return, so no thread can start a job again |
| Bluetooth (BCM4361, UART) | not known to DMA into RAM | left; the next kernel's `bt=` driver resets the chip (a phone question) |
| Display (DECON, panel) | reads 0xcc000000 every refresh | kept: the buffer is in no OS's RAM; `fb_flip`/`fb_dpp` refuse the warm restart; the loader's text and the next kernel's frames go there as after S-Boot |
| Sensor hub (SPI, its own MCU) | no DMA into RAM known | left running; the next kernel's `shub=` driver loads its firmware again (a phone question) |
| GIC, timers | interrupts | emptied and off |

## Safety: the point of no return, the watchdog, nothing written

- Nothing is written to any partition: BOOT and RECOVERY are read, through the read-only path the modem's image is read
  by. Only RAM changes. Writes to BOOT and USERDATA stay with the tester's tools.
- No Samsung code is patched or bypassed; S-Boot is not in the path because the SoC is never reset.
- Before the point of no return every failure says why and the system runs on. After it: a failure of the storage's
  stop or of a processor's `CPU_OFF` resets through the PMU (INFORM3 "unknown": BOOT, as `sweethalt`'s restart); a hang
  anywhere until the loader enters the next kernel is ended by the cluster-1 watchdog (60 s, a reset through S-Boot into
  BOOT); a bad record or payload in the loader ends in the PMU's restart into recovery after 10 s on screen. A hang in
  the next kernel before its own watchdogs is what it is on a cold boot today (the panic hook, or the keys).
- The PMU and watchdog writes are those `xnuloader/wdt_exynos.c` already makes on this phone; no PMIC or charger write.

## Recovery: iOS to TWRP, TWRP to iOS

- **iOS → TWRP (step 3, built, not run).** `warm recovery`: BOOT's slot (for the loader) and RECOVERY's kernel and
  ramdisk are staged; the loader starts the Android kernel through `ram_start` with S-Boot's FDT, which XNU's RAM has
  overwritten by then, so a cold boot with `sweetwarm=` on its line keeps a copy at 0x91100000 (the kernel refuses
  without it). The FDT is the one S-Boot prepared for BOOT (its `androidboot.*` arguments are those of a normal boot,
  not of a recovery boot); TWRP's own header line is appended. Whether TWRP's kernel runs from the state XNU left (UFS
  host, DECON, the PMIC's rails) is the phone's to say.
- **TWRP → iOS.** Linux would need `kexec_load` to start our loader. Samsung's `exynos9810-starlte_defconfig` does not set
  `CONFIG_KEXEC` (it depends on `PM_SLEEP_SMP`, default n), and TWRP for the S9 ships a kernel built from it; it sets
  `CONFIG_IKCONFIG_PROC=y`, so the answer for the TWRP actually installed is in `zcat /proc/config.gz | grep KEXEC` in
  TWRP's shell (expected: not set). Without kexec, and with no kernel or recovery rebuild, that direction stays a reset.

## What of scion-boot-exynos9810 is used

- Its **SHA-256 and CRC-32** (`src/lib/crypto.c`, MIT): in `xnuloader/sboot/warm_record.c`, with its notice.
- Its **stage-2 verification idea** (`src/loader.c` `stage2_load_and_verify`: header, CRC, SHA-256, then the jump): the
  re-entry record is checked the same way, and the record's SHA-256 of the payload plays the SCN2 header's part.
- Its **hand-off contract's shape** (`docs/handoff-contract.md`, `include/boot.h`: x0 = a versioned record, MMU off,
  D-cache clean, DAIF masked): the re-entry contract is one contract with that repository (`struct scion_reentry`).
- Not used: its PSCI/PMU core power sequencing and spin-table (it is a first stage at EL3/EL2 on a secure-boot-off
  board; the S9 runs at EL1 under Samsung's EL3, where only PSCI is available), its UART shell (the S9 has no UART
  out), its watchdog code (the facts in `xnuloader/wdt_exynos.c` were measured on this phone).

## Tests without a phone, and QEMU

- `tools/warm-restart-test.sh` (CI): the CRC-32 and SHA-256 against published values (including the one-million-`a`
  vector and a two-block tail), a record built as the kernel builds it, and every refusal (CRC, version, place, size,
  unterminated line, no direct start, Android parts missing, unknown method), under the address and
  undefined-behaviour sanitizers; the slot's sizes (`tools/warm_slot.c`: a header `image_size` larger than the slot
  taken, smaller or past `RAM_BASE` refused, a slot too large, no mark, no payload); and a BOOT image made by the real
  packer from a made-up template with the TWRP kernel's header shape and a 20 MB slot, taken end to end: the slot
  re-staged in whole pages, the record, the loader's checks of it and of the payload's SHA-256.
- Builds: the loader (`tools/ios27/s9/build-loader.sh`), STARLTE and FRANKEL (the warm restart is STARLTE's; FRANKEL
  answers the console's `warm` with a line), the 16.5 kernel (the console's new command has a stub in
  `drivers/core/hakios.stubs.c`).
- **QEMU does not reach this.** The project's QEMU targets are the Pixel 10's (`frankel`, a virt-like machine) and
  scion-boot's smoke test; none emulates the S9's loader environment (S-Boot's FDT, DECON, the Exynos PMU and watchdog,
  the UFS host) or runs STARLTE's kernel. The stub is generic AArch64 and could be exercised under `qemu-system-aarch64
  -M virt` with a hand-made loader at a test address; that is not done here. What QEMU cannot say in any case is the
  phone's: `CPU_OFF`/`CPU_ON`, the devices' state, the cluster caches.

## The plan for the phone

**Step 0 (no handoff, any time, no risk).** Boot a line with `sweetwarm=1` (with this kernel and this loader in BOOT).
Expect `STARLTE WARM: sweetwarm=1: the console's "warm" (handoff area 0x..., S-Boot FDT kept)`. Then on the console
`warm dry` (or `sysctl kern.sweetwarm_dry=1`): expect `STARLTE WARM: dry run: BOOT read and checked: kernel slot N
bytes (kernel_size; Image header image_size 0x2842000 not used for the copy), N pages listed (0x80080000..0x...),
payload at +0x..., N bytes hashed, SHA-256 xxxxxxxx..., the next kernel at 0x..., line "... sweetwarmed=1": nothing
stopped, running on`. Compare the SHA-256's first bytes with the SHA-256 of the image's kernel slot from the payload's
offset to its end on the Mac if wanted. A
`not started: ...` line says what is missing.

**Step 1 (the principle; daytime, owner present).** BOOT made with this loader and a kernel of this branch, line with
`sweetwarm=1 cpus=1` and neither `fb_flip=` nor `fb_dpp=`; boot (cold, through S-Boot: the last motor of the test).
On the console after the system is up, idle (no camera, no GPU work in progress: the console only), type `warm`.
Expect, in order:
1. `STARLTE WARM: point of no return: N pages to 0x80080000 (+ 0 more parts), payload SHA-256 ..., the next kernel at
   0x..., the watchdog armed for 60 s` — then the console disconnects (its controller is stopped).
2. On the panel (no vibration, no Samsung logo): `PROJECT STARBITE` / `WARM RESTART` / `CHECKING THE PAYLOAD'S SHA-256` /
   `STARTING XNU` / the loader's `ENTRY` and `BOOTARGS` lines, within a few seconds.
3. The console enumerates again and the new kernel's log starts with its first lines; among them
   `STARLTE WARM: this boot was started warm (sweetwarmed=1): no reset, no S-Boot`.
"Second boot reached XNU's first line without a reset" is the proof. Then `warm` again: `sweetwarmed=2`.

What a failure looks like, and why each is safe:
- `STARLTE WARM: not started: ...`: nothing was stopped; the system runs on.
- A reason on the panel under `WARM RESTART FAILED:` and `RECOVERY IN 10 S`: the loader refused the record or the
  payload; it restarts into TWRP through the PMU (S-Boot, the motor).
- A frozen panel and no console for up to 60 s, then S-Boot (the motor) and BOOT: the stub, the loader or the next
  kernel's first steps hung before the loader stopped the watchdog; the watchdog reset the SoC. S-Boot's log then reads
  `WDT_RST2`.
- The new kernel panics early: its panic path (`sweetreboot=` if on the line) or the keys, as for a cold boot.
- Nothing above writes storage. RAM is the only thing changed.

**Where it stopped, when the console is gone.** After the point of no return the console is stopped, so the kernel
writes each step as a line in its log's ring (`STARLTE WARM: step 1: the USB console stopped` ... `step 7: the staged
pages cleaned; TCR_EL1 ... SCTLR_EL1 ...; into the stub`), and right before the jump it puts the header `STARBITE: warm
restart: XNU's handoff reached the stub (no panic)` and the console's last lines into the pmsg ring (0xFED14000; TWRP:
`/sys/fs/pstore/pmsg-ramoops-0`). The loader appends `STARBITE: warm: the loader entered at EL1`, `the record is
right`, `the payload's SHA-256 is right`, `the OS placed, entering it`, or `the loader gave up: <why>`. With
`sweetreboot=` on the line an XNU panic writes `STARBITE: XNU panic (sweetreboot)` with the panic text and the same
step lines instead, and resets to TWRP at once. So when a warm restart ends in TWRP, read that file first, before
anything else boots (the next boot's loader and TWRP write the same ring):
- `XNU panic` and no `reached the stub`: the kernel panicked during the handoff; the last `step` line says where.
- `reached the stub` and no loader line: the stub or the copy (the watchdog would then reset into BOOT, not TWRP).
- `the loader gave up: ...`: the loader refused (the reason is on the panel too).
- `entering it` and still TWRP: the next kernel panicked early.
The first try on the phone (18 Nov) ended in TWRP about four seconds after `point of no return`, before these lines
existed; the ring had been overwritten by later boots when it was read.

**Step 2.** `sweetwarm_smp=1` with the line's usual eight cores: secondaries `CPU_OFF` and the next kernel's `CPU_ON`.
Look for every `SweetCPU ... PSCI CPU_ON rc=0` in the second boot. Add the GPU's stop.

**Step 3.** `warm recovery` (a cold boot with `sweetwarm=` first keeps S-Boot's FDT): TWRP without a reset.

**Step 4.** `sweetwarm=2`: every restart from iOS (the power menu) is warm; the test cycle is flash BOOT from TWRP or the
Mac's tools, then `warm` (the new BOOT is read at that moment: no reset needed to try a new kernel).

## Risks

- R1 The next kernel's drivers assume S-Boot's state: the UFS host (link up, already configured by XNU), DECON (left as
  XNU set it), the touch controller, the sensor hub, the PMIC rails XNU turned on. Each failure shows as that device
  missing in the second boot, not as a hang (most of them).
- R2 A device not stopped writes into the next kernel's memory: random corruption in the second boot. The GPU's soft
  reset is new on this path (the driver does it at its start); a GPU whose power domain is off when it is touched could
  hang the bus (the watchdog then resets). The loader's SHA-256 check covers the payload only up to the loader.
- R3 The copy with the MMU off runs at device-memory speed: up to a few seconds for 57 MiB (unmeasured); the watchdog
  allows 60 s.
- R4 `CPU_OFF` on this firmware from XNU (step 2) and the cluster L3 of the M3s.
- R5 S-Boot's checks that a cold boot passes and a warm one does not (anti-rollback counters, RPMB, the TrustZone's
  idea of the boot state) are not involved: S-Boot does not run, the secure world is not called but for PSCI. Whether
  anything in the secure world counts on a reset between two kernels is unknown.

## Not verified

Everything on the phone. That `CPU_OFF` hands a core back to `CPU_ON` from XNU at EL1 on this firmware; that the M3
cluster's L3 drops its lines when its last core is off; the copy's speed with the MMU off; how each driver of the next
kernel takes its device from XNU's state; that TWRP starts from it; TWRP's `CONFIG_KEXEC` on the installed image.
