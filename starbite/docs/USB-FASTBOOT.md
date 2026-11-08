# USB device mode and loader fastboot (UO1 + FB1)

The loader can now be a USB device. It enumerates on the Mac as **one composite device** with:

- a **CDC-ACM serial console** ("Starbite console", `/dev/cu.usbmodem*`), and
- a **fastboot interface** (`fastboot devices`), so `fastboot boot <image>` starts a test image from RAM without flashing.

It covers milestones UO1 and FB1 of `docs/STARBITE-MASTER-PLAN.md` (§4.4 UO1, §4.6).

**Status (2026-11-05): built, host-tested, NOT run on the phone.** Nothing here has touched hardware yet. Section 8 is the plan for the first test.

| | |
|---|---|
| Branch | `worktree-agent-a1510f27eae92dd03` |
| Build | `shim/starlte/build.sh` (Lima VM), `-mgeneral-regs-only`, no ABS64 relocations, no libc calls |
| Host test | `tools/usb-fastboot/run-host-test.sh`: 301 checks, passing with the Mac's clang and with gcc in the VM |
| Image cost | about 48 KB of code. The loader does nothing new unless fastboot is entered: one GPIO read and one mailbox read at loader start |

---

## 1. Files

| File | What it is | Pure (host-testable)? |
|---|---|---|
| `shim/starlte/usb_phy.c/.h` | USB clock gates, PMU isolation, and the USBCON PHY sequence | no |
| `shim/starlte/max77705_muic.c/.h` | The MAX77705 on HSI2C 0x14360000. Routes D+/D- to the AP (CONTROL1). Also reads the fuel gauge for `getvar battery-*` | no |
| `shim/starlte/usb_dwc3.c/.h` | Polling DWC3 device-mode driver: core init, event buffer, ep0 state machine, bulk TRBs, stop | no |
| `shim/starlte/usb_desc.c/.h` | Descriptors and chapter 9 / CDC-ACM control requests | **yes** |
| `shim/starlte/fastboot.c/.h` | Fastboot protocol subset, boot-image parsing, choice of boot method, the console's line shell | **yes** |
| `shim/starlte/fastboot_dev.c` | The loader side: `fastboot_run()`, `fastboot_hook()`, `fastboot_wanted()`, and the `fbp_*` platform functions | no |
| `shim/starlte/ramboot.h` + the end of `xnu_boot.c` | `star_boot_ram()`: starts an image that is already in RAM. It uses the same placements and handoff state as the catalog's start paths | no |
| `tools/usb-fastboot/host_test.c`, `run-host-test.sh` | Host harness: feeds commands and setup packets, checks responses and descriptors | — |

The change to existing code is additive only. `xnu_boot.c` gets two `#include`s, a 7-line hook before the menu, and the new `star_boot_ram()` at the end of the file. `build.sh` gets one file list.

`boot_xnu`, `boot_linux`, `boot_android`, `boot_uefi` and `boot_bare` are untouched:

- XNU goes through the same `xnu_enter()`.
- The other methods repeat the few placement steps of their catalog paths. A later refactor can share those steps.

## 2. The USB device

| | |
|---|---|
| VID:PID | `18d1:4ee0`. This is Google's fastboot ID; the `fastboot` tool matches the interface class, not the ID |
| bcdUSB | 2.00. There is no BOS and no LPM, so the link never enters L1 (the link power states that dropped Scion's cable after 3 s) |
| Speed | high speed (480 Mbit/s); full speed also works (bulk packets are then 64 bytes) |
| Strings | "Project Starbite", "Starbite loader (Galaxy S9)", the serial (S-Boot's `androidboot.serialno`, else `STARBITE`), "Starbite console", "fastboot" |
| Power | self-powered, 100 mA |

| Interface | Class | Endpoints (DWC3 physical) |
|---|---|---|
| 0 CDC-ACM control (IAD covers 0-1) | 02/02/00 | 0x81 interrupt IN, 16 bytes (phys 3), never sent |
| 1 CDC-ACM data | 0a/00/00 | 0x82 bulk IN (5), 0x02 bulk OUT (4) |
| 2 fastboot | ff/42/03 | 0x83 bulk IN (7), 0x03 bulk OUT (6) |

ACM uses protocol 0 (no AT commands), so modem managers leave the port alone. ACM IN transfers are at most 511 bytes, so no zero-length packet is ever needed. Line coding is accepted and stored; nothing depends on it.

## 3. Memory used

All of it is in the loader's non-cacheable window (`mmu.c`, 0x8c000000 and up: Normal NC). DMA therefore needs no cache maintenance; the code only places `dsb` barriers before each doorbell and after each event read.

| Range | Use | Why it is free |
|---|---|---|
| 0x8c000000..0x91000000 (80 MiB) | **download buffer** = `xnu_boot.c` STAGE. `max-download-size` = 0x04ff0000 | STAGE only holds a store image while the catalog loads one; fastboot runs before that. The XNU payload is read from here in place; a UEFI image is moved down to 0x8c000000 and entered there, as today |
| 0x91000000..0x91004000 | DWC3 driver: event buffer (4 KiB), one TRB per physical endpoint (+0x1000), setup packet (+0x1200), ep0 data (+0x1400) | Nothing in the loader uses 0x91000000..0x91200000. STAGE ends at 0x91000000; the vendor log area starts at 0x91202000; the log page is at 0x91fff000 |
| 0x91004000..0x91050000 | class buffers: fastboot command RX 512 B (+0x4000), ACM RX (+0x4400), fastboot TX 16 KiB (+0x8000), ACM TX (+0xc000), fastboot scratch / staged log / console ring / console scratch, 64 KiB each (+0x10000..+0x50000) | same |
| 0xFED17FE0 | boot-next mailbox (`SBNEXT2!` + key), read and written | `menu.c`'s, unchanged meaning |

Where each method's image goes is exactly where the catalog path puts it. Kernel: RAM_BASE + text_offset. Initrd/ramdisk: OS_RD 0xa8000000. DTB: LINUX_DTB 0xae000000 (Linux) or EDIT_FDT 0xad000000 (Android). XNU: its window via `xnu_enter`. None of these overlaps the download buffer or the USB area.

After `usbd_stop()` the DWC3 does no more DMA, so the 0x91000000 area is just RAM to whatever OS starts next.

## 4. Register sequence

### 4.1 D+/D- switch (MAX77705 Type-C MCU, I2C 0x25 on HSI2C 0x14360000)

TWRP's (and any vendor kernel's) shutdown resets the MCU, which leaves the switches open; without this step the Mac sees nothing.

1. HSI2C bring-up, as `em.c`: CMU_CMGP gates 0x14202014/2074/20a8/20ac |= MANUAL\|CG_VAL, QCH 0x14203050 |= 3, USI SW_CONF 0x14212030 = 4 (I2C), GPM14/15 function 2, S-Boot's timing registers snapshotted.
2. Read HW_REV 0x00, FW_REV 0x01, BC_STATUS 0x08 (bit 7 VBUS), USBC_STATUS1 0x06, CC_STATUS0 0x0a.
3. Opcode CONTROL1_WRITE: `[0x21, 0x06, 0x09]`, then `[0x41, 0x00]` (COM_USB: D+ and D- to the AP).
4. Read it back: CONTROL1_READ `[0x21, 0x05]` + `[0x41, 0x00]`, then poll 0x51 until the answer starts with 0x05. Up to four tries, as Scion's `hakios.max77705.c` does.
5. Every 500 ms the loop reads BC_STATUS:
   - VBUS low on two readings in a row means the cable is out.
   - When VBUS comes back, the switch is closed again at 0, 0.5, 1.5, 3 and 6 s, and the link is restarted from the PHY up. Nothing in the DWC3 notices a replug, because the PHY forces VBUS valid. Scion saw a link left as it was get resets but never another SETUP.

### 4.2 Clocks and PHY (`usb_phy.c`)

**Writes, and nothing else on the CMU:**

- `0x1a242064` GATE_CLKCMU_FSYS0_USB30DRD (CMU_TOP) |= bit 20 MANUAL | bit 21 CG_VAL
- `0x1100206c` FSYS0 USB30DRD_REF_CLK gate, same bits
- `0x11002008` FSYS0 USBDPPHY_REF_SOC_PLL gate, same bits
- PMU `0x1406072c` |= 1 (USB2 PHY out of isolation), PMU `0x14060704` |= 1 (USB3/DP)

**Read only:** CMU_TOP mux 0x1a241060 and div 0x1a241858, FSYS0 user mux 0x110001e0, mux autogate 0x11000228, Q-channels 0x11003000/304c/3050/3054/3058. They appear in `fastboot oem usb` as before → after.

**USBCON 0x11100000** (vendor CAL `phy_exynos_usb_v3p1_enable`, VER_03_0_0, as `tools/linux/port/src/usb/phy-exynos9810-usbdrd.c`):

| Step | Register | Change |
|---|---|---|
| Q-channel | LINK_CTRL +0x04 | DIS_QACT_ID0/VBUS_VALID/BVALID/LINKGATE set, FORCE_QACT clear, 500 µs, FORCE_QACT set |
| link reset | CLKRST +0x20 | LINK_SW_RST pulsed 10 µs |
| PHY reset high | CLKRST | PHY_SW_RST \| PHY_RST_SEL |
| power | HSP_TEST +0x5c | SIDDQ (bit 24) clear |
| UTMI | UTMI +0x50 | FORCE_SUSPEND, FORCE_SLEEP, DP/DM pull-downs clear |
| clocks | HSP +0x54 | EN_UTMISUSPEND \| COMMONONN |
| PHY reset low | CLKRST | 100 µs, then PHY_RST_SEL kept, PHY_SW_RST and PORT_RST clear |
| VBUS | LINK_CTRL / UTMI / HSP | bus filter bypass 0xf, FORCE_BVALID \| FORCE_VBUSVALID, VBUSVLDEXTSEL \| VBUSVLDEXT |
| OCD | LINK_PORT +0x08 | HUB_PORT_SEL_OCD_U3/U2 |
| ReWA parked | +0x100/108/10c/110/11c | as the Linux driver |
| USB3 side idle | LINK_CTRL / COMBO_PMA +0x48 | PIPE3_FORCE_EN \| FORCE_RX_ELEC_IDLE, PHY_STATUS clear; PMA_LOW_PWRN |
| tune | HSP_TUNE +0x58 | stock device-mode values (tx_vref 0xd, rise 1, res 3, pre-emp 3, sqrx 4, compdis 3) |

The routine checks that SIDDQ is clear and both FORCE_*VALID bits read back. Otherwise it stops with "USBCON did not take the PHY settings".

The PHY is **not** powered down when fastboot ends. Every OS here runs its own PHY init (Linux this same sequence; the woa UEFI saves and restores the registers).

### 4.3 DWC3 0x10c00000 (`usb_dwc3.c`)

Sources: the Linux `drivers/usb/dwc3` order, Scion's `hakios.dwc3.c` (polling, runs on this phone, read for facts), and the woa UEFI's `UsbDebugHid.c`. Where Linux and the polling ports differ, the ports' settings won; each case is noted in the code.

1. GSNPSID must read 0x5533xxxx or 0x3331xxxx, else the driver stops with "no DWC3".
2. DCTL = CSFTRST; wait for it to clear (up to 200 ms), then 10 ms. **This completes only with the PHY clock running.** "DWC3 soft reset stuck" therefore points at clocks or the PHY.
3. GUSB2PHYCFG: SUSPHY, U2_FREECLK_EXISTS, ENBLSLPM and PHYSOFTRST cleared. GUSB3PIPECTL: SUSPHY cleared. (These are the DT quirks `dis_u2_susphy`, `dis_u3_susphy` and `dis-u2-freeclk-exists`.)
4. GCTL:
   - SCALEDOWN and PRTCAP cleared, then PRTCAP = device | SOFITPSYNC.
   - DSBLCLKGTNG only on 2.10a..2.50a; it is cleared otherwise.
   - GFLADJ 30MHZ = 0x20 with SDBND_SEL (DT `quirk-frame-length-adjustment`, from 2.50a).
5. Bus settings (the same as Scion and the woa UEFI):
   - GSBUSCFG0 |= INCRBRSTEN | INCR16BRSTEN, plus REQINFO 0x2222 from 2.20a.
   - GSBUSCFG1 BREQLIMIT = 3.
   - GUCTL REFCLKPER = 0x14, plus USBHSTINAUTORETRYEN.
6. DCFG: speed high, address 0, LPM_CAP clear.
7. Event buffer:
   - GEVNTADRLO/HI = 0x91000000.
   - GEVNTSIZ = INTMASK | 4096. The interrupt is masked; events are still written and polled.
   - GEVNTCOUNT is drained.
   - DEVTEN = DISCONN | USBRST | CONNDONE.
8. ep0:
   - DEPSTARTCFG(0) on phys 0.
   - For phys 0 and 1: DEPCFG (control, max packet 512, XferComplete + XferNotReady, TX FIFO 0 for IN), then DEPXFERCFG(1).
   - DALEPENA = 3.
   - The SETUP TRB (CONTROL_SETUP, 8 bytes) is started on phys 0.
9. DCTL |= RUN_STOP (link-state request bits always masked); wait for DSTS.DEVCTRLHLT to clear. The pull-up is now on.

**Events** (polled, 4 bytes each, GEVNTCOUNT acknowledged per event):

- **Reset:** bulk transfers are ended and disabled, DALEPENA = 3, the address is reset to 0, and ep0 is re-armed if it was not in its setup stage.
- **Connect done:** DSTS speed is read; ep0 gets DEPCFG MODIFY to max packet 64.
- **ep0:**
  - The data stage TRB starts at once: phys 1 for IN; phys 0 for OUT, rounded up to 64 bytes.
  - IN data shorter than wLength that ends on a 64-byte boundary gets a zero-length packet.
  - The status stage (STATUS2 after two-stage requests, STATUS3 after a data stage) starts only on **XferNotReady(Status)**, on the endpoint the event names. It is remembered if it arrives early.
  - SET_ADDRESS goes into DCFG during the setup stage.
  - SET_CONFIGURATION(1) runs DEPSTARTCFG(2), then DEPCFG + DEPXFERCFG for the five endpoints, then DALEPENA.
  - CLEAR_FEATURE(HALT) ends the endpoint's transfer, sends CLEARSTALL, and restarts the transfer.
  - TRB status SETUP_PENDING in the data stage re-arms SETUP.
  - Unknown requests: SETSTALL on phys 0, then SETUP re-armed.
  - A 1 s watchdog restarts ep0 stuck outside its setup stage.
- **Bulk:** one TRB per transfer (NORMAL | LST | IOC, ISP on OUT; OUT lengths are a multiple of the max packet size). STARTTRANSFER with the TRB address; the resource index comes from DEPCMD[22:16]. Completion reads the TRB's remaining size.

**Stop** (before every OS start, reboot, or return to the menu):

1. ENDTRANSFER (FORCERM | IOC) on every busy endpoint.
2. DCTL RUN_STOP clear; wait for DEVCTRLHLT while draining events (soft disconnect: the Mac sees the device leave).
3. DEVTEN = 0; event interrupt masked; count drained; DALEPENA = 0.
4. DCTL CSFTRST.
5. Event buffer registers zeroed.

### 4.4 Downloads

The fastboot core runs one command per bulk OUT transfer (512-byte buffer). After `DATA<size>` has gone out, OUT TRBs point straight into the download buffer, in chunks of at most 8 MiB, each rounded up to the max packet size. The host sends exactly `size` bytes; a short packet ends the last chunk. A zero-length packet in command position is ignored.

## 5. Entering fastboot, and wiring it into other menus

### 5.1 What works now

The one hook in `star_prepare_xnu` runs after `cat_load()` and before the menu loop. `fastboot_hook()` enters fastboot when either of these holds:

- **Volume Up held alone at loader start** for 300 ms. S-Boot's own chords always include Power or Bixby; Volume Down at loader start is planned for the Emergency OS (master plan §4.3); Bixby is Engineer Lite. Whether S-Boot ignores Volume Up alone is [unverified]: check it on the first test.
- **Boot-next = `fastboot`**: the mailbox at 0xFED17FE0 holds `SBNEXT2!` + `"fastboot"`. The hook consumes it. Ways to set it:
  - `fastboot reboot bootloader` or `fastboot oem boot-next fastboot` from the Mac;
  - console `reboot fastboot`;
  - from Linux: `devmem 0xFED17FF0 64 0; devmem 0xFED17FE8 64 0x746f6f6274736166; devmem 0xFED17FE0 64 0x21325458454e4253`, then reboot. The key must end in a NUL, which is why 0xFED17FF0 is zeroed.

On the phone's screen: rows 0-5 show "STARBITE FASTBOOT", the link state, the D+/D- result, the last command, and a counter line (`RST n SETUP n CFG n STALL n <line state>`, or the download progress).

**Bixby or Power** leaves to the menu, except while a download is running.

### 5.2 API (`fastboot.h`, "the loader side")

```c
int fastboot_run(void);        /* FASTBOOT_MENU or FASTBOOT_BOOT; reboots do not return */
uintptr_t fastboot_entry(void);/* after FASTBOOT_BOOT: the entry star_prepare_xnu must return */
int fastboot_wanted(void);     /* boot-next "fastboot" (consumed) or Volume Up held alone */
uintptr_t fastboot_hook(void); /* wanted ? run : 0, the entry or 0 */
```

`fastboot_run()` needs nothing passed in: it reads S-Boot's FDT from log page word 3. It needs:

- the framebuffer on (`star_fb_enable`, done before the hook);
- `cat_load()` done (for `getvar catalog`, `oem reboot-to` and `next`).

It stops USB before it returns. With FASTBOOT_BOOT the image is already placed, and the log page holds log[10]/[15]/[17]; the caller only has to return `fastboot_entry()` from `star_prepare_xnu`. `xnu_boot.S` then does the usual handoff: D-cache clean, MMU off, x0 = log[17], S-Boot's CPACR.

### 5.3 Wiring it into the recovery menu (early boot, `xnu_boot.c` / `recovery.c`)

A "Fastboot" item:

```c
case REC_FASTBOOT:
    if (fastboot_run() == FASTBOOT_BOOT)
        return fastboot_entry();   /* from star_prepare_xnu (or hand it up to it) */
    redraw();                      /* FASTBOOT_MENU: back in the recovery menu */
    break;
```

If the recovery menu takes over the early key check, move `fastboot_wanted()`'s key test there, or keep the hook first. The hook costs one GPIO read when no key is held. The chord is one line in `fastboot_wanted()`.

### 5.4 Wiring it into the boot menu (menu v6/v7, `menu.c`)

The menu returns entry + 1 (and 0 for Download mode). Add a pick code, and handle it in `star_prepare_xnu`'s loop *before* the `pick <= 0` check:

```c
#define STAR_PICK_FASTBOOT (-2)          /* menu.h */
...
int pick = branded ? star_menu(&in, cmdline, BOOT_LINE_LENGTH) : star_ios_pick();
if (pick == STAR_PICK_FASTBOOT) {
    if (fastboot_run() == FASTBOOT_BOOT)
        return fastboot_entry();
    continue;                            /* the menu comes back */
}
```

### 5.5 Boot-next from the settings record

`tools/store/settings.py next=fastboot` writes `next_key`. `menu.c boot_next()` currently drops keys that are not catalog entries. To honour it, have `boot_next()` return STAR_PICK_FASTBOOT for `"fastboot"`. The RAM mailbox path needs nothing more: the hook handles it.

## 6. Commands

### 6.1 Fastboot

| Command | What it does |
|---|---|
| `fastboot devices` | lists the serial (`androidboot.serialno`) |
| `fastboot getvar <v>` | `version` (0.4), `version-bootloader` (starbite-fb1), `product` (starlte), `serialno`, `secure` (no), `unlocked` (yes), `is-userspace` (no), `max-download-size`, `battery-voltage` (mV), `battery-level` (%), `battery-soc-ok`, `usb-speed`, `boot-as`, `downloaded`, `catalog-count`, `catalog:<i>` (key, method, ready), `has-slot:*` (no) |
| `fastboot getvar all` / `getvar catalog` | all of the above / the catalog as `(bootloader)` lines |
| `fastboot boot <file>` | any file; `fastboot` wraps it as the kernel of a v0 boot image. **XNU** if the kernel is a `SWEETXNU` payload. **UEFI** if it is a `SBUEFI!!` bundle or an arm64 Image containing an EDK2 FV (`_FVH`). Otherwise an arm64 Image is **Linux** if a DTB came with it, **Android** if a ramdisk did (S-Boot's FDT, its bootargs + `--cmdline`), else **bare** (x0 = S-Boot's FDT) |
| `fastboot boot Image initrd.img --dtb x.dtb --header-version 2 --cmdline "..."` | Linux with initrd and DTB. The DTB must have `/chosen` `linux,initrd-*` placeholders, like the store's DTBs; `--cmdline` is appended to its bootargs |
| `fastboot oem boot-as <auto\|xnu\|linux\|android\|uefi\|bare>` | forces the method for the next boots |
| `fastboot oem log [pmsg\|dmesg\|loader\|usb]` | as `(bootloader)` lines, and staged: `fastboot get_staged out.txt` gets the whole text. `pmsg` is the ramoops pmsg ring at 0xFED14000 (the loader's, the woa UEFI's, XNU's sweetreboot log from the last boot); `dmesg` the ring at 0xFED10000; `loader` the nonzero words of the log page 0x91fff000; `usb` the full USB/PHY/MAX77705 state |
| `fastboot oem reboot-to <key>` | boot-next mailbox = key, then reboot (the menu starts it) |
| `fastboot oem boot-next <key\|fastboot>` | mailbox only, no reboot: the reset after the next boot lands there. `oem boot-next fastboot`, then `fastboot boot x` means the test OS's reboot comes back to fastboot |
| `fastboot oem catalog`, `oem usb`, `oem help` | |
| `fastboot continue` (or `oem menu`) | USB stops, then the boot menu |
| `fastboot reboot`, `reboot bootloader`, `reboot recovery` (TWRP), `reboot download` | PMU INFORM3 normal / boot-next fastboot / recovery / download |
| `flash:`, `erase:`, `flashing` | refused. FB1 has no write path at all |

### 6.2 Console

`screen /dev/cu.usbmodem<serial>1 115200`, or `cu -l /dev/cu.usbmodem…`. A banner and the `sb> ` prompt appear when the port is opened (DTR).

| Command | What it does |
|---|---|
| `help` | the command list |
| `info` | same variables as `getvar all` |
| `log [pmsg\|dmesg\|loader\|usb]` | prints the log |
| `catalog` | prints the catalog |
| `next <key>` | sets boot-next, then reboots |
| `reboot [recovery\|download\|fastboot]` | reboots there |
| `menu` | back to the boot menu |

Backspace and Ctrl-C work.

## 7. Mac side

- **Android platform-tools is installed:** `/opt/homebrew/bin/fastboot`, a link to the Homebrew cask `android-platform-tools` 37.0.1 (`fastboot version 37.0.1-15733141`). Nothing to download. (`adb` is there too, but these rules never use it on the phone.)
- **macOS 27.2:** `system_profiler SPUSBDataType` no longer exists. Use `system_profiler SPUSBHostDataType`, or `ioreg -p IOUSB -l -w0`.
- **Host test:** `sh tools/usb-fastboot/run-host-test.sh`, also with `CC=gcc` inside the VM. It covers:
  - descriptor walk at both speeds, IAD, endpoints, strings;
  - every handled control request, plus stalls for bad ones;
  - boot images v0/v2/v3 (cmdline across `extra_cmdline`), truncated or bad headers, wrapped XNU, bundle, FV, junk;
  - getvar, download (chunked), upload, oem log line-splitting, reboots and their mailbox, refusals, a cable pulled mid-download;
  - the console's editing and commands.

## 8. First hardware test plan

Division of work, as always: Claude runs every host command (build, flash through TWRP, fastboot, logs); the user only presses the phone's buttons. Explain before and after each boot.

**0. Before.** The loader with this branch must go into BOOT through the usual path: `tools/make-xnu-boot.sh`, then `tools/twrp-flash-boot.sh` with the phone in TWRP. That flash is the only write; this branch adds no write path of its own. Keep the previous BOOT image to flash back.

**1. Normal boot is unchanged.** Reboot without touching anything. The menu must come up as before (the hook only costs one GPIO read). If it does not, flash the old BOOT back and stop.

**2. Enter fastboot.** Reboot from TWRP with the USB-C cable already in the Mac, and hold **Volume Up** from the Samsung logo until "STARBITE FASTBOOT" shows. If S-Boot reacts to Volume Up (recovery or another mode instead of the loader), the chord must change. Write down what happened. The screen should then read:
   - row 3 `D+/D-: ROUTED TO THE AP`. If it says `MAX77705 DID NOT ANSWER`, the MCU switch was not set; the Mac will see nothing.
   - row 2 `USB: WAITING FOR THE HOST`, then `USB: CONNECTED (HIGH SPEED)` within about 2 s.
   - row 5 counters: `RST 1 SETUP ≥10 CFG 1 STALL ≤3 J` (or K).
   - If it says `USB FAILED:` row 4 names the stage:

     | Message | Likely cause |
     |---|---|
     | `USBCON did not take the PHY settings` | PHY block unclocked or isolated |
     | `DWC3 soft reset stuck` | no PHY clock: check the gates/PMU |
     | `no DWC3` | controller unclocked |
     | `ep0 configuration refused` | DEPCMD failure |
     | `did not leave halt` | run/stop failed |

**3. On the Mac** (Claude runs these):
   - `system_profiler SPUSBHostDataType`: expect "Starbite loader (Galaxy S9)", Vendor ID 0x18d1, Product ID 0x4ee0, the S9's serial, Link Speed 480 Mb/s.
   - `ioreg -p IOUSB -l -w0 | grep -i -A3 starbite`: the same device and its interfaces.
   - `ls /dev/cu.usbmodem*`: one port.
   - `fastboot devices`: `<serial>	fastboot`.

**4. Read how the failure looks.**

| What you see | What it points to |
|---|---|
| Nothing at all on the Mac, and row 5 shows `RST 0` with line `SE0` | no pull-up seen: D+/D- not routed (row 3; MAX77705), or the PHY not driving |
| `RST 0` with `J` | the PHY drives but the host saw no attach: cable/port, or the MUIC routes elsewhere |
| `RST`>0 but `SETUP 0` | the link is up but SETUP never reaches the event buffer: Scion's replug symptom, or a TRB/DMA problem |
| `SETUP`>0, `CFG 0`, the Mac shows an unknown device or "not functioning" | ep0 stages (status phase or descriptor answers) |
| `STALL` climbing | the host retries requests we refuse (fine in small numbers: BOS, qualifier variants) |
| Configured, but no `/dev/cu.usbmodem*` | ACM binding: fastboot can still work |
| Configured, but `fastboot devices` empty | fastboot interface or its endpoints |

**5. Protocol.**
   1. `fastboot getvar all`: every value above; battery read from the fuel gauge.
   2. `fastboot oem usb` (or `oem log usb` + `get_staged`): keep the register dump in `build/` as the first reference.
   3. `screen /dev/cu.usbmodem*`, then `help`, `info`, `log`.

**6. A download.**
   1. `fastboot boot` of something harmless that comes back: a bare test Image whose job is to reset. Or first just `fastboot stage /tmp/x` (40 MB of random data) and `fastboot oem log`, to check the download path.
   2. Time it: 40 MB should take 1-2 s at high speed.
   3. If it hangs on "Sending", the bulk OUT / TRB / DMA path is at fault. The phone's row 5 shows how many MB are still to go.

**7. FB1's done-when.**
   1. `fastboot oem boot-next fastboot`, then `fastboot boot out/xnu-sdprobe.xnu`. The probe starts from RAM.
   2. When it resets, the loader comes back into fastboot through the mailbox (if the probe resets into recovery, TWRP comes up instead; then reboot to the loader holding Volume Up).
   3. `fastboot oem log` returns the probe's pmsg log.
   4. Then `fastboot continue`: the menu.

**8. Replug.** Pull and re-insert the cable in fastboot mode. Within about 2 s the device comes back; `oem usb` counts each one under "link restarts (replug)".

**9. Each OS after fastboot.** `fastboot continue`, then start Linux from the menu. The ECM+ACM gadget must still enumerate: the DWC3 was soft-reset and the PHY is re-inited by Linux. Also check one XNU and the woa UEFI. This proves the stop leaves the controller as each OS expects.

## 9. Riskiest assumptions (untested on hardware)

1. **Clocks.** Only the three gates are written. This relies on S-Boot (or the reset state) leaving CMU_TOP's USB30DRD mux/divider and FSYS0's user mux on a running source. Linux gets away with the same gates after TWRP, so it should hold. If the soft reset sticks, compare `oem usb`'s top.mux/div and fsys0.mux with Android's values (mux 1, div 7, user mux bit 4). Scion's `usb_gadget_clk=1` shows how to set them; it is not done here, because of the rule against touching CMU muxes.
2. **MAX77705.**
   - The CONTROL1 opcode protocol and the HSI2C driver are proven in Linux and Scion respectively. This bare-metal copy writes the MCU for the first time from the loader.
   - If the MCU answers but CONTROL1 does not read back 0x09, `oem usb` shows "written, NOT read back".
   - Charging must keep working: COM_USB is the normal USB path.
   - This is the S9 (starlte); the Linux port's DTS is the S9+'s. The MAX77705 address and bus are the same in em.c's S9 driver.
3. **Cache/DMA.**
   - All DMA memory is in the loader's Normal non-cacheable window. Writes are ordered with `dsb st` before every DWC3 register write, and reads with `dsb sy` after an event count or TRB completion.
   - This assumes no system-level cache and no SysMMU in front of the DWC3. S-Boot's own download mode and Linux (no `iommus` on the node) suggest neither exists.
   - The DWC3 takes 64-bit pointers; all addresses here are below 4 GiB anyway.
4. **The ep0 status stage on XferNotReady** is the databook/Linux/Scion way. The woa UEFI starts it at once instead. If enumeration stalls at `SETUP n, CFG 0`, this is the first suspect. The 1 s ep0 watchdog keeps it from wedging for ever.
5. **Volume Up at loader start** being ignored by S-Boot [unverified].
6. **Images that are not page aligned:**
   - `fastboot` puts the kernel at offset 2048 (page size 2048). XNU payloads are read in place (8-byte alignment is enough). UEFI images are moved to 0x8c000000. arm64 Images are copied to RAM_BASE + text_offset.
   - A boot image with page size 2048 and an XNU payload over 80 MiB - 64 KiB will not fit.
7. **Fastboot's behaviour assumed from AOSP sources:** wrapping non-boot-image files, the default header v0, page 2048. The host test checks the parser against a hand-built v0/v2/v3 layout, not against bytes made by `fastboot` itself. The first real `fastboot boot` is the first true check.

## 10. Later

- flash to **store entries only** (disabled by default, never a protected partition; master plan §4.6);
- the reset-without-SETUP restart heuristic Scion uses for a replug nobody saw;
- the XNU UFS log ring in `oem log`;
- moving the driver into libsb for the Utility OS (UO2+);
- a shared finalize between `boot_*` and `star_boot_ram`.

## 9. First hardware run (2026-11-05, S9 #1)

Entered from the Bixby recovery menu (USB FASTBOOT / CONSOLE). First try:
- the Mac (macOS 27.2) asked to allow "Starbite loader (Galaxy S9)"; then `system_profiler SPUSBHostDataType`
  showed it at 480 Mb/s, 18d1:4ee0, serial <serial>; `/dev/cu.usbmodem101`; `fastboot devices` listed it.
- `getvar all`: battery 100 % / 4324 mV, 23 catalog entries; ep0: 20 SETUPs, 0 stalls, 0 command failures.
- `fastboot stage` 40 MiB of random data: 0.945 s (~43 MB/s); `downloaded` matched. Register dump:
  build/usb/oem-usb-first.txt. MAX77705: D+/D- routed (CONTROL1 read back 0x09), 4 re-routes, 0 I2C errors.
- `oem boot-next fastboot` + `fastboot boot out/xnu-sdprobe.xnu`: "booting as xnu", the probe ran from RAM to its
  end (card, MBR, store CRCs, read 22.7 MB/s, write test 5.0 MB/s) and panicked deliberately as it does. The panic
  reset went to TWRP (sweetreboot), not to fastboot: a test kernel's reset path wins over the boot-next mailbox.
  The log came from TWRP's /sys/fs/pstore/pmsg-ramoops-0. FB1's done-when is met (log read via pstore).
