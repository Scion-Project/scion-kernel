# Galaxy S9 microSD under iOS 16.5 (`sdcard=`), read only

26 November 2026. Off by default. Not yet run on a phone.

`drivers/storage/sd/hakios.exynos_sd.c` and `hakios.sdmedia.cpp` are Jesvi-iOS's microSD driver for the Galaxy S20
(jesvi-eulah, `redwood_sdcard.c` at b35cfdc and `RedwoodSDMedia.cpp` at 0b6bce9, APSL-2.0), moved to the S9's controller
and made read only. Jesvi's design notes are `docs/devices/x1slte/sdcard.md` of Jesvi-iOS. Scion's own design for a
card driver that brings the card up itself (rails, identification, a root on the card) is
[XNU-MICROSD.md](../ios27/XNU-MICROSD.md). This driver is the smaller of the two: it uses a slot that something before
the kernel already initialised, as S-Boot does on the S20.

## How the two compare

| | Jesvi-iOS (S20, Exynos 990) | this driver (S9, Exynos 9810) | XNU-MICROSD.md (Scion, iOS 26/27 forks) |
|---|---|---|---|
| controller | dwmmc2 0x132E0000, DW MSHC 2.70a, 64-bit FIFO | dwmmc2 0x11500000, DW MSHC 2.70a, 64-bit FIFO (DT fifo-depth 64; XNU-MICROSD 2.3 step 6) | the same |
| bring-up | none: S-Boot leaves the card in tran | none: used only if found initialised | full (gates, pins, S2MPS18 LDO31/LDO2 via ACPM, CMD0/8/ACMD41/2/3/9/7/ACMD6) |
| reads | PIO, CMD17/18 with auto-stop, 64 KB a command | the same code | PIO, CMD17/18, 128 KB, recovery with CMD12/CMD13 |
| writes | CMD24 on the first partition (sd_write=) | none: no write command in the file | none in v1 |
| BSD side | /dev/disk9, disk9s1 (char) | the same, read only | devsw `sdroot0` for a root |
| IOKit | IOBlockStorageDevice -> IOMedia disk9 -> disk9s1, External | the same, Writable no | none |
| Files | lifs.kext loaded by the kernel, a userfsd wrapper, LiveFiles prefs | not yet (below) | not a goal |

## Boot arguments

| argument | what happens |
|---|---|
| `sdcard=0` (default) | nothing |
| `sdcard=1` | a look, **nothing written**: the card-detect pin (GPA1-5, low with a card), the card's three clock gates (CMU top 0x1a242070, FSYS1 0x11402030 and 0x11402034), and, only if both FSYS1 gates read forced on, the controller's VERID, HCON, PWREN, CLKENA, CLKDIV, CTYPE, CLKSEL, STATUS, CMD and RESP0 |
| `sdcard=2` | as 1, the three gates forced on (bit 20 manual, bit 21 on, read back; XNU-MICROSD.md 2.3 step 2), then, with a card in and the slot as a loader leaves an initialised card (VERID x270a, HCON data width 2, PWREN 1, CLKENA 1, CTYPE 1), the MBR read 6 s after start and `/dev/disk9`, `rdisk9`, `disk9s1`, `rdisk9s1` and the IOMedia published, read only |

`kern.hakios.sdcard` says the state in one line; `sdcard_reads`, `sdcard_errors`, `sdcard_dbg` and `sdcard_last_int` say
more. Nothing writes to the card; the PMIC, the pins and the clock dividers are not touched.

## What is not known yet

Whether anything before the kernel leaves the S9's card initialised. S-Boot does on the S20; on the S9 nobody has looked.
Scion's loader has a card reader of its own (`xnuloader/sboot/sd.c`); if a boot read the card through it, the slot may
be left so. `sdcard=1` answers this in one boot. If the slot is not initialised, the next step is the card's own
bring-up from XNU-MICROSD.md section 2.3, written for this kernel. That is a separate change.

Files: iOS 16.5's external-drive path (userfsd and livefileproviderd) needs Apple's `lifs.kext`. Scion's kernel does
not load it. Jesvi-iOS does (its `redwood_kextload.c`, d838091, and the userfsd start-up wrapper, 6139189). Until that
is ported, the card is visible as `/dev/disk9s1` and in the IORegistry, but not in Files.

## Test plan (an S9, iOS 16.5 image from this branch, a FAT32 or exFAT card)

1. `sdcard=1`. In the log: `sdcard: sdcard=1: card detect GPA1 DAT 0x.. (a card); gates top 0x.., FSYS1 ACLK 0x..
   SDCLKIN 0x..`, then either `the controller's clocks are not on ...: not touched` (the answer: send it), or the
   `dwmmc2: VERID ... RESP0 ...` line and `slot initialised` or `not as a loader leaves an initialised card`.
   Take the card out and boot again: `no card`.
2. Only if step 1 said `slot initialised`: `sdcard=2`. After about 6 s: `sdcard: ready: disk9 N MB, disk9s1 type 0x..
   at ..., N MB (read only); IOKit published`. From the console: `sysctl kern.hakios.sdcard`, and
   `dd if=/dev/rdisk9s1 bs=512 count=1 | xxd | head` shows the partition's boot sector (`EXFAT` or `FAT32`).
   `ioreg -l | grep -A3 'SD Card'` shows the device. Writing (`dd of=/dev/rdisk9s1`) must fail with EROFS.
3. Stop if `sdcard_errors` grows quickly, or the log shows a hang after the `dwmmc2:` line; send the log.

To revert: boot without `sdcard=`. Nothing is written to the card; the gates forced on with `sdcard=2` are clock
gates, back to S-Boot's setting at the next boot.

## Credits

Jesvi-iOS (github.com/jesvi-eulah/Jesvi-iOS, by jesvi-eulah). The commit that brings its files in keeps its author;
see NOTICE.
