# XNU microSD root driver (design)

Status: v1 (PIO, read-only) implemented as patches/xnu-12377/starlte patches 0345/0346 and built (section 12),
with the review fixes (error recovery, failed-mount panic, early bring-up print, store-aware `lba:`).
The probe payload `out/xnu-sdprobe.xnu` is built but not flashed or booted. The phone was not touched.

The goal is to let our XNU ports (iOS 26 on xnu-12377, iOS 14 on xnu-7195, both STARLTE) mount an
HFSX root from the 128 GB microSD card. The driver is read-only by default. That frees internal UFS,
which is full today (`docs/IOS14.md:179-185`), and it is a precondition for giving Android 24 GB of UFS.

Sources read for this design. Every register and step below cites one of them.

| Source | What it gives |
|---|---|
| `xnuloader/sboot/sd.c`, `sd.h` | the loader's polling, read-only reader: clocks, pins, rails, controller init, SD init, CMD17/18 PIO |
| `xnuloader/sboot/menu.c:1695-1716` | `hw_sd_rails`: the S2MPS18 LDO31/LDO2 power cycle |
| `NOTES.md:1125-1141` | Linux milestone 2: what the card controller needed on this phone |
| `NOTES.md:1961-1962` | the 128 GB card (SK128, 249999360 sectors) and its MBR |
| `tools/linux/mk-starlte-dts.py:139-215` | the Linux DT node that works (clocks, ciu-div 3, timings, 50 MHz cap) |
| `tools/linux/patch-mainline.py:60-125` | the three dw_mmc fixes (dma-32bit, error logging, max data timeout) |
| Linux 7.1 `drivers/mmc/host/dw_mmc*.{c,h}` and `core/sd*.c` | in the VM at `/mnt/build/src/mainline` (`tools/linux/build-kernel.sh:33`) |
| `build/linux/runs/journals-20261103/*/system.journal` | Linux dmesg from the Arch and Kali boots: the CMD5/CMD52 lines, bus speeds |
| `build/xnu-12377/bsd/dev/herolte_ufs.c` | our STARLTE UFS root: devsw, `rd=ufsroot0`, herolog, sweetreboot (patches 0204, 0206, 0300) |
| `build/xnu-12377/bsd/dev/a5xelte_emmc.c` | our earlier dw_mmc driver (Exynos 7580 eMMC, PIO) and the console ring |
| `build/xnu-12377/iokit/bsddev/IOKitBSDInit.cpp:684-1118` | `IOFindBSDRoot` with the STARLTE hook |
| `build/xnu-12377/iokit/Kernel/IOService.cpp:5396-5470` | why there is no IOMedia in our kernels |
| `docs/ANDROID-PLAN.md:260-330`, `tools/store/sbstore2.py`, `xnuloader/sboot/store.h` | the card layout, now and after the Android re-layout |

`build/xnu-12377` is the series applied in full. Its `git log -1` is patch 0344.

---

## 1. Corrections to the brief (read these first)

1. **There is no IOBlockStorageDevice to subclass.** Our kernels ship no IOStorageFamily. The port
   patches out the asserts that expect it: `IOService.cpp:5400` (`assert(iomediaClass)` skipped on
   SWEET/A5XELTE, patches 0022/0103), `:5411` ("SWEET has no IOStorageFamily: without the IOMedia
   class no media exists to unhide") and `:5463` (no IOBlockStorageDriver class). STARLTE builds
   with `-DARM64_BOARD_CONFIG_A5XELTE` (`makedefs/MakeInc.def:101`), so those branches apply to it.
   Every root driver in the port is therefore a **BSD block/char devsw**, not IOKit. That covers
   `herolte_ufs.c:748-772` and `1273-1283`, `sweet_ufs.c` (patch 0028) and `a5xelte_emmc.c`.
   `IOFindBSDRoot` returns the `dev_t` directly and jumps to `iofrootx` (`IOKitBSDInit.cpp:830-841`).
   The design follows that proven pattern. Section 5 says what an IOKit nub could still add.
2. **The loader does not run the card in high-speed mode, and 22.6 MB/s is the Linux figure.**
   `sd.c` initialises at 400 kHz and switches to "25 MHz" with no CMD6 (`sd.c:16`, `251`, `287`).
   The 22.6 MB/s comes from mainline Linux: IDMAC, SD high speed, a 200 MHz bus clock with
   CLKDIV 2 = 50 MHz (`NOTES.md:1136`). The journals show the same: "Bus speed = 200000000Hz
   (req 50000000Hz, actual 50000000HZ div = 2)" and "new high speed SDXC card at address d555".
   NOTES has no record of the loader reader (`sd.c`, commit 381b2ae) running on hardware. The
   hardware path is proven by Linux. The exact `sd.c` sequence is not yet proven.
3. **The CMD5 errors are harmless.** The journals show `starbite: CMD5 arg 0x00000000 error -110
   rintsts 0x00000100 resp 0x000001aa` and two `CMD52 ... error -110 rintsts 0x00000100`. These
   come from the mmc core's SDIO probe of a memory-only card. RINTSTS bit 8 is RTO, a response
   timeout (`dw_mmc.h:376`), which is the expected answer. `resp 0x1aa` is the stale R7 of the
   CMD8 before it. The `starbite:` print is the error logger from `patch-mainline.py:78-95`. The
   XNU driver sends neither command: its init is SD-memory-only, like `sd.c:255-288`.
4. **The card has no free space for a new partition today.** p1 (8192..182886399) and p2
   (182886400..249999359) cover the whole card. 249999359 is its last sector (`NOTES.md:1961-1962`).
   A "p3" would come out of p2. That is the Android plan's re-layout (`docs/ANDROID-PLAN.md:300-316`).
   Section 6 picks a range that is valid both before and after that re-layout.

---

## 2. Controller bring-up, mirroring sd.c exactly

The driver is `bsd/dev/starlte_sd.c`, under `#if defined(ARM64_BOARD_CONFIG_STARLTE)`, with one
line in `bsd/conf/files` next to `herolte_ufs.c` (patch 0204, `bsd/conf/files`). The same file
goes into series-7195, whose `0300-starlte-board-target.patch` has the same STARLTE block in
`IOFindBSDRoot`.

### 2.1 Addresses (all from sd.c, nothing new)

| What | Address | Source |
|---|---|---|
| Controller (DW MSHC 2.70a, Exynos7 type) | 0x11500000, size 0x2000 | `sd.c:24`, DT `mk-starlte-dts.py:189` |
| CMU top GATE_CLKCMU_FSYS1_MMC_CARD | 0x1a242070 | `sd.c:7`, `217` |
| FSYS1 MMC_CARD ACLK / SDCLKIN gates | 0x11402030 / 0x11402034 | `sd.c:7`, `217` |
| DIV_CLKCMU_FSYS1_MMC_CARD (**read only**) | 0x1a241864 | `sd.c:8-10`, `221` |
| pinctrl_fsys1 GPF1 CON / PUD / DRV | 0x11430000 + 0x0 / 0x8 / 0xc | `sd.c:75`, `223-225` |
| Card detect GPA1 DAT (bit 5, active low) | 0x14050044 | `sd.c:76`, `210` |
| Rails: S2MPS18 LDO31 / LDO2 (PMIC type 1, regs 0x68 / 0x4a) | via ACPM | `menu.c:1702-1716` |
| IRQ (not used in v1) | SPI 249 | `NOTES.md:1126`, `mk-starlte-dts.py:190` |

The controller window comes from a new ADT node `/starbite-sd` with `reg = <0x11500000 0x2000>`.
It is added in `tools/ios27/mkadt_starlte.py` next to `herolte-ufs` (`mkadt_starlte.py:243-245`) and
looked up with `SecureDTLookupEntry`, as `herolte_ufs.c:1215-1220` does. The side registers (CMU,
pinctrl, GPA1) are constants, as in `sd.c`. Each one gets its own `ml_io_map(page, PAGE_SIZE)`,
the pattern in `herolte_ufs.c:1024-1025`.

**MIF and INT clocks are never touched.** The only clock registers written are the three gates
above. They are set the same way `sd.c:216-220` and the Linux DT `s9p,clock-gates`
(`mk-starlte-dts.py:148-153`) set them. The CMU divider is only read. Linux writes it to /1 through
its DT (`mk-starlte-dts.py:154-156`); `sd.c` deliberately leaves it (`sd.c:9-10`), and so does XNU.

### 2.2 Register map (sd.c:24-72; matches Linux dw_mmc.h)

CTRL 0x00, PWREN 0x04, CLKDIV 0x08, CLKSRC 0x0c, CLKENA 0x10, TMOUT 0x14, CTYPE 0x18, BLKSIZ 0x1c,
BYTCNT 0x20, INTMASK 0x24, CMDARG 0x28, CMD 0x2c, RESP0 0x30 (+4 per word), RINTSTS 0x44,
STATUS 0x48, FIFOTH 0x4c, VERID 0x6c, HCON 0x70, BMOD 0x80, CLKSEL64 0xa8, DATA 0x200.

- CMD bits (`sd.c:47-57`; Linux `dw_mmc.h:386-402`): START 31, USE_HOLD_REG 29, UPD_CLK 21,
  INIT 15, PRV_DAT_WAIT 13, SEND_STOP (auto stop) 12, DAT_EXP 9, RESP_CRC 8, RESP_LONG 7, RESP_EXP 6.
- RINTSTS bits (`sd.c:59-72`; Linux `dw_mmc.h:367-384`): RE 1, CD 2, DTO 3, RCRC 6, DCRC 7,
  RTO 8, DRTO 9, HTO 10, FRUN 11, HLE 12, SBE 13, ACD 14, EBE 15. The data-error mask is
  DCRC|DRTO|HTO|FRUN|SBE|EBE (`sd.c:72`).
- FIFO count is STATUS[29:17] (`sd.c:312`; Linux `SDMMC_GET_FCNT`, `dw_mmc.h:404`). Card busy is
  STATUS bit 9 (`sd.c:165`; `dw_mmc.h:406`).

### 2.3 Sequence (port sd_up() step for step)

Every wait has a deadline on `mach_absolute_time()`, as `herolte_ufs.c:253-281` does, in place of
`sd.c`'s generic-timer `deadline()` (`sd.c:116-134`). The timeouts are the same as `sd.c`'s.
Every failed step records its name, like `sd_rep.why`, and the driver prints
`STARBITE SD: <step> failed (rintsts 0x.. status 0x..)`.

| # | Step | sd.c |
|---|---|---|
| 1 | Card present: GPA1 DAT bit 5 low, else ENXIO "no card" | `210-214` |
| 2 | Open the three gates: `v \| (1<<20) \| (1<<21)` (manual, on) | `216-220` |
| 3 | Read the CMU divider; bus_hz = 806 MHz / ((div & 0xf) + 1) / 4 | `221-222`, `74` |
| 4 | Pins: CON = (CON & 0xff000000) \| 0x00222222 (GPF1-0..5 func 2); PUD &= 0xff000000; DRV = (DRV & 0xff000000) \| 0x00333334 | `223-225` |
| 5 | Rails: the `hw_sd_rails` sequence through ACPM (2.4) | `226-228`, `menu.c:1702-1716` |
| 6 | Wait 10 ms; VERID & 0xffff must be 0x270a; HCON[9:7] must be 2 (64-bit FIFO) | `229-235` |
| 7 | CTRL \|= 7 (controller + FIFO + DMA reset), wait for the bits to clear (100 ms) | `237-239` |
| 8 | BMOD = 0; CTRL &= ~(bit 25 USE_IDMAC \| bit 4 INT_ENABLE); INTMASK = 0; RINTSTS = ~0 | `240-243` |
| 9 | **TMOUT = 0xffffffff.** Without it every read ends in DRTO (rintsts 0x20c), even at 400 kHz | `244`, `NOTES.md:1133-1135`, `patch-mainline.py:110-125` |
| 10 | FIFOTH = (2<<28) \| (31<<16) \| 32 | `245` |
| 11 | CLKSEL64 = (3<<24) \| (2<<16): ciu div 3, drive phase 2, sample phase 0 | `246` |
| 12 | CTYPE = 0 (1 bit); PWREN = 1; wait 5 ms | `248-250` |
| 13 | Clock 400 kHz: CLKENA=0, update, CLKDIV, CLKSRC=0, update, CLKENA=1, update. Each update is CMDARG=0, CMD = START\|HOLD\|UPD_CLK\|PRV_WAIT, wait for START to clear; div = ceil(bus/(2·hz)), max 255 | `136-160`, `251-253` |
| 14 | CMD0 with INIT | `256-257` |
| 15 | CMD8 arg 0x1aa, R1 flags; RESP0 & 0xfff must be 0x1aa | `258-261` |
| 16 | ACMD41 (CMD55 rca<<16 first) arg 0x40ff8000, RESP_EXP only (R3 has no CRC), every 10 ms for up to 1.5 s, until OCR bit 31; high capacity = OCR bit 30 | `197-201`, `262-272` |
| 17 | CMD2 (long, CRC): CID from RESP0..3 | `273-276` |
| 18 | CMD3: rca = RESP0 >> 16; CMD7 rca<<16 (select) | `277-281` |
| 19 | CMD16 512 only if not high capacity | `282-283` |
| 20 | ACMD6 arg 2 (4 bit), CTYPE = 1 | `284-286` |
| 21 | Clock 25 MHz (same update dance), BLKSIZ = 512 | `287-289` |

Commands themselves follow `sd.c:163-187`. First wait up to 500 ms for card-not-busy. Then:
RINTSTS = ~0; CMDARG; CMD = START\|HOLD\|PRV_WAIT\|flags\|idx; wait 100 ms for START to clear. Then
wait 500 ms for CD or HLE. HLE gives -1, RTO -2, RCRC (only with RESP_CRC) -3, RE -4. USE_HOLD_REG
is always set, as in `sd.c:169`. `a5xelte_emmc.c:145-147` sets it only when CLKSEL's drive phase
is non-zero, and here the drive phase is 2.

**The clock comes out below 25 MHz when the divider is /1.** bus_hz is 806/4 = 201.5 MHz. The
round-up division then gives CLKDIV 5, so 20.15 MHz. Linux reports exactly 200 MHz because its DT
declares the clock as 800 MHz (`mk-starlte-dts.py:179-183`). The driver prints the CMU divider,
bus_hz and card_hz once, so the first boot shows what S-Boot or the loader left.

### 2.4 Card power from XNU

The loader's `hw_sd_rails` (`menu.c:1702-1716`) works as follows:
1. Read LDO31 (0x68) and LDO2 (0x4a). Refuse when LDO31's vsel is outside 0x24..0x3c and not 0.
2. Write 0x68=0x28 and 0x4a=(l2 & 0x3f), which turns both rails off.
3. Wait 20 ms.
4. Write 0x68=0xe8 (2.8 V on) and 0x4a=0xfc (3.3 V on).
5. Wait 5 ms.

XNU already has this PMIC path: `starlte_pmic_read` / `starlte_pmic_write(type 1 = PMIC, reg, val)`
in `bsd/dev/starlte_touch.c:372-394` (patch 0302). They are serialised by `acpm_lock`
(`starlte_touch.c:138`) and use the same command encoding as the loader's `pmic_write`
(`menu.c:448-455`). The driver repeats the five steps exactly, with the same vsel check. The power
cycle also returns a card the loader left mid-transfer, or that the loader's own store reader
used, to idle (`menu.c:1697-1700`). No other PMIC register is touched.

### 2.5 Reads (sd.c:295-358)

Reads use `read_chunk`:
1. FIFO reset (CTRL \|= 2, wait), then BLKSIZ 512 and BYTCNT n·512.
2. arg = sector if high capacity, else sector·512. Send CMD17 for one sector, or CMD18 with
   SEND_STOP (auto CMD12) for more.
3. Drain the FIFO: on each pass read RINTSTS, then FCNT, then copy FCNT 64-bit words from DATA.
4. Stop on a data error (-5). Finish on DTO with all words in. DTO with an empty FIFO and words
   missing is a short transfer (-6). The deadline is 200 + 2·n ms (-7).
5. For CMD18, wait for ACD (-8).

Chunks are at most 256 sectors (128 KiB), and each chunk gets 3 tries (`sd.c:339-349`).

- **No bounce buffer.** PIO writes straight into the `buf_map()`ed buffer, unlike
  `herolte_ufs.c:621-655`, which needs one for DMA. Buffers that are not 8-byte aligned (never seen
  from HFS, but physio can pass anything) go through a 512-byte stack sector.
- **FIFO access width.** `sd.c:314` uses one 64-bit load. Linux uses `dw_mci_pull_data64`
  (`dw_mmc.c:2410`). The 32-bit-pair quirk (`DW_MMC_QUIRK_FIFO64_32`) is for the 7870 only
  (`dw_mmc-exynos.c:141-145`). Our 7580 eMMC driver did need 32-bit pairs
  (`a5xelte_emmc.c:160-164`), so boot-arg `sdfifo32=1` keeps that as a fallback.
- RXDR is not acknowledged while draining. `sd.c` does not acknowledge it, and with INTMASK = 0 it
  only latches. Linux and `a5xelte_emmc.c:175-177` do acknowledge it. If the first test shows the
  FIFO stall at the watermark, writing RINTSTS = RXDR after each drain is the first change to try.

### 2.6 High speed (v1.1, behind `sdhs=1`, off by default)

Version 1 matches `sd.c` exactly, at 25 MHz nominal. High speed is a separate later step, taken
once v1 is proven:
1. CMD6 is an adtc read of 64 bytes (BLKSIZ 64, BYTCNT 64) with arg 0x80fffff1: mode set, group 1,
   function 1. Linux builds it as `mode << 31 | 0x00FFFFFF`, with the group nibble replaced
   (`core/sd_ops.c:363-377`).
2. Check `status[16] & 0xf == 1` (`core/sd.c:401-406`).
3. Only then set the clock to 50 MHz.

CLKSEL stays as it is. For SD high speed, Linux `set_ios` falls through to `sdr_timing`
(`dw_mmc-exynos.c:331-364`). That value is `SDMMC_CLKSEL_TIMING(0, 2, 3)` from the DT `<0 2>` and
ciu-div 3 (`dw_mmc-exynos.c:396-405`, `dw_mmc-exynos.h:20-29`), which is the value `sd.c:246`
already writes.

The divider is a problem here. At bus_hz 201.5 MHz, a ceiling division for 50 MHz gives CLKDIV 3,
so 33.6 MHz. CLKDIV 2 would give 50.4 MHz, just above the card's limit. Linux runs CLKDIV 2 because
it believes the clock is 800 MHz. The driver keeps the ceiling division, and boot-arg
`sdclkdiv=N` can force a value for tests.

---

## 3. PIO vs IDMAC

**v1 is PIO**, as in `sd.c` and `a5xelte_emmc.c`:
- No DMA memory and no cache maintenance.
- No 32-bit DMA constraint. Linux's `starbite,dma-32bit` exists because IDMAC transfers failed
  with the descriptors in the 0x8_8000_0000 bank (`patch-mainline.py:62-76`, `NOTES.md:1131-1133`).
- The host cannot scribble on RAM.

The cost is CPU time. The bus delivers at most about 12.5 MB/s at 25 MHz on 4 bits, and the
64-bit drain keeps up with that easily. A read blocks its thread for about 6-10 ms per 128 KiB
chunk under the driver mutex. Preemption stays enabled, and the same rules apply as for the UFS
mutex (`herolte_ufs.c:621`).

**IDMAC (v2)** follows Linux, and every piece is known:
- 64-bit descriptors `struct idmac_desc_64addr`: des0 control, des2 size (13-bit buffer 1 size),
  des4/5 buffer, des6/7 next (`dw_mmc.c:55-74`).
- Ring init and end-of-ring (`dw_mmc.c:488-518`).
- IDSTS64 0x90 cleared; IDINTEN64 0x94 = NI\|RI\|TI; DBADDRL/U 0x88/0x8c (`dw_mmc.c:538-546`,
  `dw_mmc.h:322-325`, `421-427`).
- CTRL bit 25 USE_IDMAC and BMOD bit 7 ENABLE / bit 1 FB (`dw_mmc.h:342`, `429-431`).

Two rules come from our own drivers:
- Descriptors and buffers stay below 4 GiB. Use `kmem_alloc_contig(..., max_pnum = atop(4 GiB),
  ...)` (`osfmk/vm/vm_kern_xnu.h:591-599`).
- Flush and clean around each transfer, as `herolte_ufs.c:159-169` (`flush_dcache64` /
  `clean_dcache64`) does. The controller is not coherent.

IDMAC is worth doing only once PIO and high speed work. Linux shows the ceiling is 22.6 MB/s with
both.

## 4. Interrupts vs polling

**v1 polls**:
- INTMASK = 0 and CTRL INT_ENABLE = 0 (`sd.c:241-242`).
- SPI 249 is never routed in the GIC, so the driver needs no interrupt plumbing. No `bsd/dev`
  driver in the port takes one today: UFS polls UTRLDBR (`herolte_ufs.c:266-282`).
- To avoid burning a core on long reads, copy the UFS driver's `ufswait=1` mode
  (`herolte_ufs.c:256-279`): spin for 50 us, then `assert_wait_deadline` + `thread_block` in
  200 us slices. Use it only when interrupts are on, outside interrupt context, at preemption
  level 0, and with the mutex still held. The boot-arg is `sdwait=1`, off at first.

Interrupts would only matter with IDMAC under real load. They are not planned.

---

## 5. Kernel object layout: devsw, not IOBlockStorageDevice

As section 1 explains, the IOKit storage classes do not exist in our kernels. The layout copies
`herolte_ufs.c`:

```
bsd/dev/starlte_sd.c
  static struct { regs, side maps, rca, high_capacity, bus_hz, card_hz,
                  first_sector, nsectors, writable, ready, bmajor, cmajor, bdev } sd;
  LCK_MTX starlte_sd_lock                      one command at a time (herolte_ufs.c:144-145)
  sd_hw_up()          section 2.3 (once)
  sd_read_sectors(lba, n, va)    section 2.5, absolute card sectors, bounds-checked
  sd_find_range(spec)            section 6
  bdevsw {open, close, strategy, ioctl, psize, D_DISK}     (herolte_ufs.c:748-756)
  cdevsw {read = physio(strategy), write = EROFS, ioctl}   (herolte_ufs.c:733-772)
  devfs nodes "sdroot0" / "rsdroot0", 0640 root:operator   (herolte_ufs.c:1282-1283)
  int  starlte_sd_rootdev(dev_t *)   for IOFindBSDRoot     (herolte_ufs.c:1388-1399)
  void starlte_sd_probe(void)        boot-arg sdprobe=1    (herolte_ufs.c:1298)
```

- **open**: ENXIO unless ready. EROFS for FWRITE unless `sdrw=1` (`herolte_ufs.c:580-586`).
- **strategy**: offset = blkno · 512, and the range must fit inside the partition
  (`herolte_ufs.c:600-611`). Writes return EROFS unless writable. Each chunk of at most 256
  sectors is read under the mutex. `buf_setresid` after each chunk, then `buf_biodone`.
- **ioctl**: a fixed 512-byte sector, so `DKIOCSETBLOCKSIZE` only accepts 512.
  - `DKIOCGETBLOCKSIZE` 512, `DKIOCGETPHYSICALBLOCKSIZE` 512, `DKIOCGETBLOCKCOUNT` nsectors.
  - `DKIOCISWRITABLE` = writable.
  - `DKIOCGETMAXBLOCKCOUNTREAD` 256, `...WRITE` 0 while read-only.
  - `DKIOCGETMAXSEGMENTCOUNT*` 1.
  - The same set as `herolte_ufs.c:661-713`.

"Media publish" in this port therefore means the devfs nodes plus the `dev_t` returned to
`IOFindBSDRoot`, which is what `ufsroot0` does. An optional later step is a plain IOService nub
(`StarbiteSDHost`) registered in the IOKit plane with VERID, CID, bus and card clocks and the root
range as properties, like the `iokit/Kernel/arm/Sweet*.cpp` services in patch 0300. It would be for
`ioreg` only and carries no I/O. Real IOMedia would need IOStorageFamily, and that is out of scope.

## 6. Which sectors: `sdroot=`

There is no default. With `rd=sdroot0` and no `sdroot=`, the kernel panics with a clear message,
so a mistyped command line can never expose the wrong area.

- **`sdroot=pN`**: an MBR partition.
  - Read sector 0 and check 0x55aa at 510/511. Primary entries are at 446 + 16·i: type at +4,
    LBA at +8, count at +12 (`sd.c:360-382`).
  - N = 1..4 are primary partitions.
  - N >= 5 are logical partitions. They are found by walking the EBR chain inside the extended
    entry (type 0x05 / 0x0f), with relative LBAs, at most 32 hops. The chain must stay inside the
    container. This is needed for the Android re-layout, where p3 is an extended container
    (`ANDROID-PLAN.md:309-316`).
  - Refused: an empty entry, type 0x07 (exFAT, the user's backups), type 0xDA (the OS store, which
    `sd.c:375` and `store.c` use), and a range past the card end (249999360 sectors).
  - The intended type for an iOS root partition is 0xAF (Apple HFS). vold mounts only
    FAT/NTFS/exFAT types (`ANDROID-PLAN.md:318`), so Android leaves it alone.
- **`sdroot=lba:START,COUNT`**: an absolute range in 512-byte sectors.
  - START and COUNT must be multiples of 8, for 4 KiB alignment, and must fit the card.
  - Read-only (v1, built): the range must not overlap any FAT/exFAT/NTFS partition, primary or
    logical (types 0x01/0x04/0x06/0x07/0x0b/0x0c/0x0e, the whole EBR chain is read; an unreadable
    table refuses). Where it overlaps a 0xDA store entry it must lie inside that entry and start at
    or beyond the store's used end, read from the SBSTORE2 header (p2 block 1: u64 capacity at 16,
    u64 used blocks at 24, CRC at 48; `store.h:9-16`). A bad header CRC or an SBSTORE1 view with no
    SBSTORE2 header refuses. An 0xDA entry holding no store at all is allowed.
  - Writable (v1.1, not built): additionally the range must lie inside one MBR entry other than the
    data partitions.
  - Only `sdroot=pN` refuses 0xDA as such; `lba:` may sit in the store's unused tail, which is where
    the test range is.

The driver prints the result. For example:
`STARBITE SD: root lba 249868288 +131072 (64 MiB), inside p2 type 0xda, read-only, card SDXC rca 0xd555 20150 kHz`.

## 7. `rd=sdroot0` in IOFindBSDRoot

The hook goes inside the existing STARLTE block of `IOFindBSDRoot`. In xnu-12377 that block is
`IOKitBSDInit.cpp:803-841`, right after the `ufsroot0` case (`830-841`). The `rd=` value is parsed
at `IOKitBSDInit.cpp:735-737`. Series-7195 gets the same hunk in its STARLTE block (0300).

```c
	{
	uint32_t sdprobe = 0;                       /* read-only: card, MBR, sdroot= range head */
	PE_parse_boot_argn("sdprobe", &sdprobe, sizeof(sdprobe));
	if (sdprobe) {
		extern void starlte_sd_probe(void);
		starlte_sd_probe();
	}
	}
	/* rd=sdroot0: the sdroot= range of the microSD card, read-only unless sdrw=1. */
	if (strcmp(rdBootVar, "sdroot0") == 0) {
		extern int starlte_sd_rootdev(dev_t *dev);
		OSSafeReleaseNULL(matching);
		int sd_error = starlte_sd_rootdev(root);
		if (sd_error) {
			panic("IOFindBSDRoot: STARLTE microSD root %s unavailable: %d", rdBootVar, sd_error);
		}
		strlcpy(rootName, rdBootVar, rootNameSize);
		IOLog("BSD root: %s, major %d, minor %d\n", rootName, major(*root), minor(*root));
		*oflags = 0;
		goto iofrootx;
	}
```

At `iofrootx` (`IOKitBSDInit.cpp:1118-1120`), `setRootMedia(service)` runs with `service == NULL`,
as it already does for `ufsroot0`. `vfs_mountroot` then mounts HFS on the `dev_t`. It mounts
read-write only with `rootrw=1` (`bsd/vfs/vfs_subr.c:1253-1264`), so the first tests never pass
`rootrw`. With `rd=sdroot0` the UFS driver is never initialised. Nothing on UFS is read or
written, and `ufsrw` has no effect.

`herolte_log_start` (`herolte_ufs.c:1113-1139`) does more than the log: it also starts
`starlte_ufs_heartbeat` and `sweet_sample_start`. The SD init must start the sampler itself on a
read-only root, which is what `herolte_ufs.c:1122-1127` does.

## 8. Logs

| Where | When | Source |
|---|---|---|
| Console ring, 4 MiB RAM, every console byte | always | `a5xelte_log_append` (`a5xelte_emmc.c:627-638`) from `_cnputs` (`osfmk/console/serial_console.c:294-300`); `a5xelte_log_snapshot` (`a5xelte_emmc.c:641-648`) |
| Framebuffer console | always | the photographed boot screen |
| ramoops pmsg at 0xFED14000 ("DBGC" header, 0x3F00 bytes), read in TWRP as `/sys/fs/pstore/pmsg-ramoops-0` | on any panic once `sweetreboot=N` is set | `herolte_ufs.c:937-1043`; armed at `STARTUP(KMEM)`, independent of UFS |
| "HEROLOG1" ring on flash | **only with sdrw=1, v1.1** | format `herolte_ufs.c:776-850` |

The first tests write nothing to flash. The pmsg ring is RAM, which the read-only rule allows.
`starlte_panic_to_recovery` (`herolte_ufs.c:990-1019`) writes the panic head and then the console
ring tail into it, and resets to TWRP. So `sdprobe=2` ends the probe with a deliberate
`panic("STARBITE SD: probe done")`, and the whole probe log arrives in TWRP with no button presses
beyond the usual.

When writes come (v1.1), the card log uses the herolog layout unchanged:
- Header block "HEROLOG1" \| u32 version \| u32 ring size \| u64 total \| u64 seq, then the 4 MiB
  ring, in the last 1 + 1024 4 KiB blocks of the `sdroot=` range (`herolte_ufs.c:851-859`).
- `tools/ios27/starlte-log-pull.py` then needs only a card offset option.
- The HFSX volume must end 4 MiB + 4 KiB before the range ends, the same rule as for UFS
  (`docs/IOS14.md:129-147`).
- The write path is CMD24/CMD25 with TXDR filling and a wait for card-not-busy after the data,
  as `a5xelte_emmc.c:198-302` does (bit 9 at `289-297`). Writes are allowed only inside the range
  and only with `sdrw=1`.
- The writer should become a backend of one shared log routine (`write_blocks`, `sync`) rather
  than a copy of `herolte_log_write`.

## 9. First device test: read-only, tiny HFSX image

**Where the image goes: `sdroot=lba:249868288,131072`, the last 64 MiB of the card.**
- 249868288 + 131072 - 1 = 249999359, the card's last sector. 249868288 / 8 = 31233536, so the
  range is 4 KiB aligned.
- Today it is the tail of p2. Its offset in the store is 8372736 blocks, against an 8389120-block
  capacity. It is safe as long as the store's `used` stays below that.
- After the Android re-layout it falls inside "236169216..249999359 free in p3"
  (`ANDROID-PLAN.md:316`).

The same command line therefore works across the re-layout. Before writing, confirm with
`tools/store/store-pull.sh ... sd` + `sbstore2.py header` that the store's used end clears
block 8372736 (it prints `microSD root tail ...: clear`). `sbstore2.py build --medium sd` refuses a
store that would reach the last 16384 blocks of p2 (`SD_ROOT_TAIL_BLOCKS`, like `HEROLOG_BLOCKS` for
UFS), so a future store build cannot grow into the range. The driver also refuses the range when
the card store's used end reaches past its start (section 6).

Host (Mac, small: 64 MiB):
1. Make a raw volume with no partition map: `hdiutil create -size 64m -fs "Case-sensitive HFS+"
   -layout NONE -volname SDTEST`. It is HFSX and **not journaled**, so a read-only mount never
   needs a journal replay. This is how `tools/build-ios-root.sh:51-77` makes roots, minus the
   journal.
2. Put in the static test init from the iOS 14 kernel test (`MDROOT=build/ios14/sweet-init`,
   `docs/IOS14.md:59`) as `/sbin/launchd`.
3. Add a known file `/sdtest.bin` (16 MiB random) and its SHA-256.
4. Record the sha of the whole raw image.

Phone (later; the user only presses buttons, Claude runs TWRP over adb):
1. `dd` the 64 MiB image to `/dev/block/mmcblk0` at seek 249868288 (512-byte blocks), read it
   back, and compare the sha.
2. Nothing else on the card changes.

Boots:
- **T1, probe on a RAM-disk root.** Use the iOS 14 kernel test command line
  (`tools/cmdline/ios14-kernel-test.txt`, `rd=md0`) plus `sdprobe=2 sdroot=lba:249868288,131072
  sweetreboot=60`. The probe:
  1. Brings the card up (section 2.3) and prints every `sd_rep`-style field.
  2. Reads and decodes the MBR.
  3. Reads sector 2 of the range, the HFS volume header at byte 1024, and checks the `H+` / `HX`
     signature.
  4. Times a 32 MiB read in 128 KiB chunks.
  5. Panics on purpose.

  The log arrives in pmsg. Pass means every step is OK, the signature is `HX`, and the throughput
  is printed.
- **T2, root on the card.** `rd=sdroot0 sdroot=lba:249868288,131072` with no `rootrw`, no `sdrw`
  and no `ufsrw`. HFS mounts the volume read-only and launchd (the test init) runs. It lists `/`,
  hashes `/sdtest.bin` and prints the result. Pass means the hash matches the host.
- **T3, a real root.** An iOS 14 root (8 GiB) needs a range of 8 GiB + 8 MiB, which is more than
  the 6.6 GiB free tail. Two places would work:
  - the tail of today's 32 GiB p2, before the Android re-layout;
  - a logical 0xAF partition, if the Android plan gives up space (for example a smaller p9).

  Either way the layout decision belongs to the user. It runs read-only first, then `sdrw=1
  rootrw=1` with the card herolog (v1.1).

The same kernel keeps `rd=ufsroot0` working, so nothing about the daily iOS 26 boot changes.

## 10. Risks

1. **No IOMedia.** Anything in userland that expects an IOMedia for the root (DiskArbitration,
   mount by UUID) gets nothing. The `ufsroot0` root has the same property and boots iOS 26, so
   the risk is known and accepted.
2. **The sd.c sequence is unproven on hardware.** Linux proved the controller, pins, rails,
   timings and the TMOUT fix. T1 is built to show exactly which step fails (`sd_rep.why` style).
3. **Clock rate unknown.** The CMU divider is whatever S-Boot or the loader left, and XNU does
   not write it. The card may run at 20 MHz or less. That is slow but correct. High speed is
   separate (2.6), and 50 MHz needs `sdclkdiv=2`, which is slightly over spec.
4. **Data timeout.** TMOUT must be ~0 (`NOTES.md:1133-1135`). Getting this wrong looks exactly
   like "card dead" (DRTO, rintsts 0x20c).
5. **ACPM contention.** The rails go through the same ACPM channel as touch, DVFS and the GPU
   rail. The calls are serialised by `acpm_lock`, and a failed ACPM call fails the init cleanly.
   The power cycle is safe because the card is not in use by anything else at IOFindBSDRoot time.
6. **The user's data on p1.** The driver refuses p1 in every mode, refuses the store partition
   as `pN`, and writes nowhere without `sdrw=1`. The first tests pass no write flag at all, and
   the strategy returns EROFS.
7. **Journaled HFSX on a read-only device.** A journal that needs replay cannot be replayed.
   The test image has no journal. A real root must be cleanly detached (`hdiutil detach`) before
   it is written to the card.
8. **Speed for real roots.** PIO at about 10-12 MB/s against UFS means slow app starts. Linux is
   already "bound by the microSD" at 22.6 MB/s (`NOTES.md:1179`). High speed and IDMAC are the fix,
   in that order.
9. **FIFO width.** 64-bit loads should work on the 9810 (Linux runs `pull_data64`). If T1 shows
   garbage, `sdfifo32=1` uses 32-bit pairs, as on the 7580.
10. **Card removal.** It is not handled: CD is checked once at init. Pulling the card under a
    mounted root ends in EIO and a panic, which sweetreboot turns into a TWRP reset with a log.
11. **Store overlap.** Until `sbstore2.py --medium sd` reserves the tail, a very large store build
    could grow into the test range. Writes to the range are not enabled in v1, so the image would
    be damaged, not the store. The probe's signature check would catch it.
12. **Build disk.** The Mac has about 7.5 GiB free (`df` today). Reuse `build/xnu-12377` and the
    7195 tree, build incrementally, and remove intermediates afterwards. Do not start a fresh
    clone.

## 11. Work items, in order

1. `bsd/dev/starlte_sd.c` (PIO, read-only, sections 2-6), the `bsd/conf/files` line, the
   `IOFindBSDRoot` hunk (7) and the `/starbite-sd` ADT node in `tools/ios27/mkadt_starlte.py`. These
   become a new patch in series-12377 and one in series-7195.
2. A host unit test of the MBR/EBR and range parser (compiled for macOS, fed synthetic MBRs and
   the real card layouts from NOTES and ANDROID-PLAN).
3. The tail reservation in `sbstore2.py --medium sd` (done: `SD_ROOT_TAIL_BLOCKS`, `sbstore2.py header`).
4. The T1 kernel image and the 64 MiB test image. Then T1, then T2.
5. v1.1: `sdhs=1`, `sdrw=1` with the card herolog. v2: IDMAC below 4 GiB.

## 12. Implementation (v1) and the first device test

### 12.1 What was built

| Piece | Where |
|---|---|
| Driver | `patches/xnu-12377/starlte/0345-starlte-microsd-driver.patch`: `bsd/dev/starlte_sd.c` + its `bsd/conf/files` line |
| Root hook | `patches/xnu-12377/starlte/0346-starlte-microsd-root.patch`: `IOFindBSDRoot` STARLTE block, `sdprobe=` before the UFS probe, `rd=sdroot0` next to `ufsroot0` |
| Kernel | `out/kernel.development.starlte-12377` (incremental `tools/ios27/build-xnu-12377.sh`, no warnings in the new files); the kernel before it is kept as `out/kernel.development.starlte-12377.pre-sd` |
| Payload tool | `tools/ios27/mkpayload-ios26.sh NAME CMDLINE` -> `out/xnu-NAME.xnu` (strip -x kernel + `out/starlte-iphone11.adt` + cmdline, `mkpayload.py`; a store entry, no BOOT image) |
| Probe cmdline | `tools/cmdline/ios26-sdprobe.txt`: the daily set with `rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 ufsmulti=1` replaced by `rd=sdroot0`, plus `herolog=0 sdprobe=2 sdhold=60 sdbench=16 sdroot=lba:249868288,131072 sweetreboot=1` (564 of 607 bytes). Should the probe ever return (sdprobe=1), the root is the card range, whose missing HFS header panics (EFTYPE): UFS is never brought up |
| Payload | `out/xnu-sdprobe.xnu`, 17,215,232 bytes. `mkpayload-ios26.sh` refuses a cmdline over 607 bytes or of more than one line (mkpayload.py would cut it silently), reads the packed cmdline back, and deletes its stripped kernel copy |

Differences from the design, all on the safe side:
- **No write path at all in v1.** `sdrw=1` only prints that writes are not in this build. The strategy
  returns EROFS for every write, open refuses FWRITE, and `DKIOCISWRITABLE` is 0. The card
  herolog (section 8) waits for v1.1.
- **CMD9 added** between CMD3 and CMD7 (stand-by state) to read the CSD. It gives the card size
  (249999360 sectors expected for the SK128), so reads past the end are refused and `sdroot=` is
  checked against the real size. If CMD9 fails, `sdroot=` is refused, and the probe still runs.
- `sdroot=lba:` refuses overlap with any FAT/exFAT/NTFS partition, primary or logical (EBR chain), not
  only p1, and inside the 0xDA store it must start at or past the store's used end (section 6).
  Only `sdroot=pN` refuses 0xDA as such; `pN` also refuses the data types, extended containers and
  empty entries.
- `rd=sdroot0` checks for an `H+`/`HX` volume header at sector 2 of the range before it publishes
  the devfs nodes. With no header it panics (`EFTYPE`, 79) instead of handing HFS garbage.
- The controller address is the constant 0x11500000. An ADT `/starbite-sd` node is optional and only
  checked (a node at another address refuses to start). `mkadt_starlte.py` is unchanged, so the
  daily ADT is reused as it is.
- `sdhs`, `sdwait`, `sdclkdiv` and IDMAC are not in v1. `sdfifo32=1` is.
- A failed read never leaves the controller inside the CMD18. `read_chunk` returns on a data error
  (DCRC/DRTO/HTO/FRUN/SBE/EBE) or its deadline without draining the FIFO, and flow control then stops
  the card clock. So every failure, the last retry included, runs `sd_recover()`, as Linux does after
  a data error (`dw_mci_data_complete` -> `dw_mci_reset`, `send_stop_abort`): controller + FIFO reset,
  RINTSTS cleared, UPD_CLK without PRV_WAIT, then CMD12 with stop_abort (bit 14, no busy wait, no
  PRV_WAIT; a card already in tran does not answer), then CMD13 until the card is in tran (at most
  250 ms). Each one prints `read S+N: E (rintsts .. status ..), reset + CMD12: card state 4 (tran),
  retry`. The loader's `xnuloader/sboot/sd.c` has the same recovery (compile-checked, not rebuilt
  into an image).
- `rd=sdroot0`: a second `starlte_sd_rootdev()` call means HFS did not mount (bsd_init calls
  IOFindBSDRoot again only after `vfs_mountroot` failed, and this path skips the 10-attempt check). It
  panics `STARBITE SD: HFS did not mount sdroot=... (lba .. +..)` instead of looping forever, so
  sweetreboot brings the log to TWRP.
- Bring-up order: card detect (GPA1 CON +0x40 and DAT printed on "no card"), gates and divider, then a
  `gates ... cmu div ... gpa1 con ...` line before anything else, and a gate whose bits 20/21 do not
  read back set stops with `clock gates did not open` before 0x11500000 is touched. The rails print
  before and after. VERID/HCON and S-Boot's CLKSEL print right after the first controller read.
  CLKSEL is changed in its timing fields only (`& ~0x07070007`, as Linux
  `dw_mci_exynos_set_clksel_timing`).
- The probe tells a reread mismatch (`reread DIFF`) apart from a failed reread (`reread err N`) and a
  bench that never read the first chunk (`reread skipped`), and counts recoveries (`rec N`).
- Not done yet: the series-7195 port and a committed host test of the MBR/EBR parser (section 11,
  items 1-2; the parser was host-tested against the real and the re-layout tables during review).

### 12.2 What the probe does (`sdprobe=2`)

It runs in `IOFindBSDRoot` before anything touches UFS, so with `sdprobe=2` UFS is never
initialised: nothing on internal storage is read or written. It writes nothing to the card.
1. Card up (2.3 + CMD9). It prints the gates, the CMU divider, bus Hz and the GPA1 pin, then LDO31/LDO2
   before and after the power cycle, then VERID/HCON and S-Boot's CLKSEL, then CID name, OCR, RCA,
   the size, the new CLKSEL and the card clock.
2. MBR (sector 0). It prints the disk id and every entry.
3. p1 sector 0 (the exFAT boot sector). It prints the OEM name `EXFAT   `, the volume length, the
   sector and cluster shifts, the serial and the first 16 bytes.
4. p2, the 0xDA store. It reads blocks 0-1 (16 sectors, one CMD18) and prints `SBSTORE1`, then the
   SBSTORE2 version, count, capacity, used and the **header CRC check**. Then it reads the
   directory (32 sectors, one CMD18), checks the **directory CRC** and prints up to 12 entry
   names. Both CRCs are computed by sbstore2.py on the Mac, so "ok" proves multi-block reads
   return the right bytes.
5. `sdroot=lba:249868288,131072` is resolved and checked. Then sector 249868290 (the last 64 MiB
   of the card) is read for an HFS header. Today that range is the unused tail of p2, so expect
   `sdroot ok no HFS`. It proves the range parser and a read near the card's last sector.
6. Throughput: it reads 16 MiB from p2's start in 128 KiB CMD18s, then reads the first chunk again
   and compares the two (`same`, `DIFF`, `err N` for a failed reread).
7. It prints `STARBITE SD: probe done: <summary>` and `holding 60 s ... photograph the screen now`.
   Then it waits 60 s and calls `panic("STARBITE SD: probe done (sdprobe=2, nothing written): <summary>")`.
   With `sweetreboot=1` the panic hook copies the panic head and the console tail into the
   ramoops pmsg ring and resets into TWRP.

On a failure, the summary reads `FAIL at <step> (<err>, rintsts 0x.. status 0x..) verid 0x..
ldo31 0x.. gates .. .. .. gpa1 ..`. The panic still comes, and so does the reset.

### 12.3 Device test T1 (probe only, read-only, no card re-layout)

The phone starts in TWRP (adb `recovery`). Claude runs every host command, and the user only presses
the buttons (`device-workflow-division`). The payload goes in as a **store entry on UFS, never as
BOOT**: BOOT and the daily `@boot` iOS 26 entry stay as they are.

1. **Check the payload** on the Mac:
   ```
   shasum -a 256 out/xnu-sdprobe.xnu      # e2478d7883b1b1915e425673250660d14c91d1c47399cb976fd93d9ff7cfd992
   strings out/xnu-sdprobe.xnu | grep -o 'sdprobe=2 sdhold=60.*sweetreboot=1'
   ```
2. **Pull the live store** (read-only), in the same sitting as the flash:
   ```
   tools/store/store-pull.sh build/store/ufs-store-live.img    # must not print STOP
   ```
3. **Make a catalog** from the live one plus a hidden lab entry:
   ```
   cp tools/store/catalog.toml build/store/catalog-sdprobe.toml
   cat >> build/store/catalog-sdprobe.toml <<'TOML'

   [[entry]]
   key = "sdprobe"
   name = "microSD probe"
   version = "XNU 12377 · read only · panics"
   category = "lab"
   method = "xnu"
   tag = "lab"
   files = ["sdprobe.xnu"]
   root = "none (probe ends in a panic)"
   flags = ["hidden"]
   TOML
   ```
   If store-pull.sh printed STOP, append the entry to a copy of the TOML that the live catalog was
   built from, not to `catalog.toml`. There must be no `cmdline =` line: the cmdline is 564 of
   the payload's 607 bytes.
4. **Build and compare**:
   ```
   python3 tools/store/sbstore2.py build build/store/ufs-store-sdprobe.img --medium ufs \
       --catalog build/store/catalog-sdprobe.toml --import build/store/ufs-store-live.img \
       sdprobe.xnu=out/xnu-sdprobe.xnu:xnu
   diff <(python3 tools/store/sbstore2.py list build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py list build/store/ufs-store-sdprobe.img)
   diff <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-sdprobe.img)
   ```
   The first diff may show only the changed `catalog` line and the added `sdprobe.xnu`. The second
   may show only `> sdprobe`. Anything else means stop.
5. **Flash the store**: `tools/store/store-flash.sh build/store/ufs-store-sdprobe.img`. It checks the
   SHA-256 on the phone and keeps the settings block.
6. **Card in place.** The 128 GB card must be in the slot. Nothing on it changes.
7. **User:** reboot from TWRP into the menu, then choose Engineer > Start an OS > **microSD probe**.
8. **Expected on screen** (framebuffer console, `-v`), within about 10 s of the kernel start:
   ```
   STARBITE SD: probe (read only): microSD controller 0x11500000
   STARBITE SD: gates 0x..3..... 0x..3..... 0x..3....., cmu div 0x.. -> bus 201500000 Hz, gpa1 con 0x........ dat 0x..
   STARBITE SD: rails ldo31 0x.. -> 0xe8, ldo2 0x.. -> 0xfc
   STARBITE SD: verid 0x....270a hcon 0x..., clksel 0x... (as S-Boot left it)
   STARBITE SD: card SDHC/SDXC "SK128" mid 0x.. rca 0x.... ocr 0xc0ff8000, 249999360 sectors (122069 MiB), clksel 0x3020000 (was 0x...), 20150 kHz 4 bit, fifo 64
   STARBITE SD: MBR p1 type 0x07 ... lba 8192 +182878208 ...
   STARBITE SD: MBR p2 type 0xda ... lba 182886400 +67112960 ...
   STARBITE SD: p1 sector 0: oem "EXFAT   " ...
   STARBITE SD: p2: block 0 "SBSTORE1"|..., SBSTORE2 v2, N entries, ... header crc ok
   STARBITE SD: p2: directory (one 32-sector CMD18) crc ok
   STARBITE SD: sdroot=lba:249868288,131072: inside p2 (type 0xda), past the store's used end (block N of 8389120)
   STARBITE SD: sdroot=lba:249868288,131072: 0, lba 249868288 +131072, volume header none
   STARBITE SD: read 128 x 128 KiB from 182886400: 0, ... us, ~9000-11000 KB/s, reread same, ... reads, 0 recoveries
   STARBITE SD: probe done: card up in ... ms, 122069 MiB, 20150 kHz, rca ....; p1 07 ... p2 da ...
   STARBITE SD: holding 60 s, then a deliberate panic (photograph the screen now)
   ```
   (The VERID, divider, clock and name values are expectations, not measurements.) If the card store
   was never written, p2 prints `no SBSTORE2 header`. That is still a pass for the driver as long as
   the MBR and p1 lines are right.
9. **User: photograph the screen** during the 60 s hold. The phone then panics and resets into TWRP
   by itself. If it does not reset (the screen shows the panic and stays), photograph that too, then
   do a forced reset into TWRP.
10. **Pull the log** (Claude, in TWRP):
    ```
    adb exec-out cat /sys/fs/pstore/pmsg-ramoops-0 > build/sdprobe-t1-pstore.bin
    strings build/sdprobe-t1-pstore.bin | grep -E 'STARBITE (SD|: XNU panic)'
    ```
11. **Clean up** (optional): rebuild the store from a fresh pull without the entry, or leave the
    hidden entry for T2.

**Pass:** the card comes up, the MBR matches section 1 item 4, p1 says `EXFAT   `, both store CRCs
are `ok` (or the store is honestly absent), the reread is `same`, and the throughput is printed.
A `read S+N: ... reset + CMD12: card state 4 (tran), retry` line is a recovered transient error: the
driver works, but note it. Repeated ones on every read point at the bus (clock, CLKSEL phase).
**First things to try on a failure** (one boot each):
- `clock gates did not open`: compare the three gate values with the loader's EngineerMode microSD
  page; the controller was not touched.
- `FIFO`/data errors -5/-6: `sdfifo32=1`.
- DRTO: check that TMOUT reads back ~0.
- `ACPM`: the card rails. Compare with the loader's EngineerMode microSD page.
- `no card`: reseat the card.

### 12.4 Device test T2 (root on the card, read-only), after T1 passes

Before the `dd`: pull the card store (`tools/store/store-pull.sh ... sd`) and check
`python3 tools/store/sbstore2.py header IMG` prints `microSD root tail ...: clear` (T1's
`sdroot=` line already says `past the store's used end`). Card stores are built with
`--medium sd`, which keeps the last 64 MiB of p2 free.
Make the 64 MiB non-journaled HFSX image of section 9 and `dd` it to sector 249868288 from TWRP,
then read it back and compare the sha. Then build the payload with the daily cmdline, but with
`rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 ufsmulti=1` replaced by `rd=sdroot0 sdroot=lba:249868288,131072`
and `herolog=0 sweetreboot=1` added (`tools/ios27/mkpayload-ios26.sh sdroot-t2 FILE`). Flash it as a
second hidden store entry the same way. Pass: the test init's hash of `/sdtest.bin` matches the host.
If HFS does not mount, the kernel panics `STARBITE SD: HFS did not mount sdroot=...` on bsd_init's
second IOFindBSDRoot call, after `cannot mount root, errno = N`, and sweetreboot resets into TWRP
with both lines in pmsg.
