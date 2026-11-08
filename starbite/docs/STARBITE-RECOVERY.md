# Starbite Recovery, Rescue mode and POST

Written 2026-11-05. Item 6 (Bixby interrupt, Starbite Recovery, Rescue mode) and the POST from
item 4 of `docs/STARBITE-2-PLAN.md` "Current order".

Sources: `shim/starlte/recovery.c`/`.h`, `post.c`/`.h`, `xnu_boot.c` (`star_prepare_xnu`, the
rescue start), `tools/rescue/`. **None of this has run on the phone yet.** It builds cleanly
(`-Werror`, no ABS64 relocations). Everything else below comes from reading the code.

## 1. The boot sequence

```
xnu_boot.S       entry, log page, stack, FP/SIMD                        (unchanged)
star_prepare_xnu
  MMU on                       mmu.c, tables only, cannot fail          (unchanged)
  star_fb_enable               DECON trigger 0x1281                     (unchanged)
  ── BIXBY WINDOW ─────────────── 400 ms, key GPIO + timer + fbtext only  recovery.c
     held → Starbite Recovery menu (nothing below has run yet)
  latch dump to pmsg           RAM only                                 (moved after the window)
  rescue request?              boot-next mailbox, RAM only; taken and cleared
  star_splash                  menu assets, overlay / v5-v6 intro       (unchanged)
  EL1 / MMU checks → fatal()                                            (unchanged)
  BOOT payload + bundles       RAM only
  rescue requested → start the rescue system (no POST, no menu); failure → recovery menu
  ── POST (quick) ──────────────── top of the panel, rows put back after  post.c
     fatal check failed, or Bixby held during POST → recovery menu
  cat_load                     already done by POST (cached)
  boot menu (v7 / v6 / v5 / text)                                        (unchanged)
```

Up to the Bixby check the loader uses only:
- the generic timer (`CNTPCT`, `CNTFRQ`; 26 MHz when S-Boot leaves it at 0),
- the ALIVE GPIO data registers,
- CPU writes to the framebuffer that S-Boot already lit.

Every wait in `recovery.c` also stops after a fixed number of loop turns (about 200 per µs), so
even a stopped counter cannot hang the window. In the worst case the window then lasts a few
seconds. The disassembly confirms what runs before the window: `star_mmu_enable`,
`star_fb_enable`, then `rec_bixby_window`, which calls only `fbtext` (`star_fb_row_base`,
`star_fb_row_y`, `star_fb_text_c`), `r_keys` and `copy_words`.

## 2. Keys

| Key | GPIO (active low) | Use |
|---|---|---|
| Bixby | GPA0 DAT 0x14050024 bit 6 | hold at boot: Starbite Recovery; also selects once released |
| Volume Up | GPA0 bit 3 | move up |
| Volume Down | GPA0 bit 4 | move down |
| Power | GPA2 DAT 0x14050064 bit 4 | select |

- **When to hold Bixby**: hold Bixby *alone* after S-Boot's splash appears, or from power-on.
  S-Boot's own combinations run before the loader: Vol Up + Bixby + Power opens Samsung/TWRP
  recovery, Vol Down + Bixby + Power opens Download mode.
- **Detection**: three reads 10 ms apart must all see Bixby down (about 30 ms). If Bixby is held
  from the start, the menu shows within about 30 ms.
- **Late presses**: POST also checks Bixby after each of its checks. A press during POST, or
  during POST's 1.5 s pause after a red line, opens recovery after POST.
- **Before the boot menu**: the recovery menu waits until every key is released before it returns,
  and menu v7 ignores keys that are already held when it starts (`in_ignore_held`). The Power
  press that chose Continue does nothing in the boot menu.

## 3. Starbite Recovery menu (`recovery.c`)

The menu is plain fbtext: the 5x7 font at 6x scale, 38 columns, black background. It needs no
assets. Volume Up/Down move the highlight, Power selects.

| Item | What it does | Touches |
|---|---|---|
| Continue normal boot | puts back the panel as it was on entry, then the boot goes on (POST, menu) | nothing |
| Starbite Rescue mode | checks that a rescue system exists, writes the mailbox, resets (§5). If none exists: a red reason, and the menu stays | RAM (BOOT bundle); UFS + store if there is no bundle |
| Engineer Lite | `em_main` (EngineerMode) with BOOT's payload info; a pick made there is started | menu assets (font atlas), whatever its pages touch |
| Run POST (full) | the long POST (§4), listed in the menu, then a key | as POST, plus RTC and touch |
| Reboot to TWRP | INFORM3 0x12345674 + SWRESET | PMU |
| Reboot to Download mode | INFORM3 0x12345671 + SWRESET | PMU |
| Reboot | INFORM3 0x12345670 + SWRESET | PMU |
| Power off | PS_HOLD low (`hw_power_off`). With a cable in, the phone stays on and the menu says so | PMU |
| Boot entry... | the whole catalog, hidden entries included (grey: files missing, gold: hidden). Starts the pick; a failure comes back here with the reason | UFS, store, catalog |

Each item touches storage or peripherals only after the user picks it. A start that fails, from
Boot entry or from Engineer Lite, comes back to the recovery menu with the reason in red. Every
action is written to the pmsg ring (`STARBITE: recovery: ...`) and to log slot 216:
`"RECV" << 32 | count << 16 | item`.

On entry the menu saves a full copy of the panel (S-Boot's splash), so Continue gives menu v7's
intro the same splash it would have captured. If an OS load was attempted in between, the copy
may have been overwritten (`rec_panel_lost`). Continue then leaves a black panel, which v7's
intro also handles (`I7_CAPT_BLACK`).

## 4. POST (`post.c`)

### Quick POST: every boot, after the Bixby window

Results are drawn at the top of the panel (rows y 200..980) in the style of a PC BIOS:
`✓`/`✗`/`!`, name, detail, time.
- Those rows are saved first (0x9E000000) and put back afterwards, so the boot menu's intro still
  captures S-Boot's splash.
- A red line pauses the boot 1.5 s. Gold lines and skipped checks do not.
- Only the RAM and timer checks are fatal. If one fails, the recovery menu opens with the reason
  instead of the boot menu.

| Check | Quick | Full | Cap / bound | Fail (red) | Warn (gold) |
|---|---|---|---|---|---|
| RAM | 1 MiB at 0xA0000000, address and ~address | 64 MiB, + 0x55.. / 0xAA.. | size; warn over 60 ms / 8 s | any bad word (**fatal**) | slow |
| TIMER | counter moves (100k-read bound), CNTFRQ | ticks between two PMIC RTC second edges | loop counts; 3 s full | counter stopped (**fatal**) | off by >3 % from 26 MHz; RTC unread |
| PMIC | ACPM PMIC channel + S2MPS18 ID (TOP 0x00), read | same | acpm_send's 100 ms waits | no channel / no answer | over 250 ms |
| UFS | `ufs_up` | same | ufs.c: 2 s per command, at most 9 (18 s on a dead device, which the menu already paid) | host not ready / busy / refused / no answer | over 2 s |
| GPT | `gpt_load` + USERDATA present | same | one read | unreadable, no USERDATA | slow |
| STORE | `store_open(UFS)`: header CRC + directory CRC | same | a few reads | no store / bad CRC | empty |
| CATALOG | `cat_load`: the store's catalog, CRC-checked | same | one entry | the store has one but its CRC/format is bad (built-in in use) | no catalog in the store |
| BATTERY | MAX77705 RepSOC (read only) | + VCELL mV, current mA | 3 ms per I2C message, 40 ms per check (120 ms full) | bus or fuel gauge no answer | below 10 % |
| TEMP | TMU: two big-core probes + GPU | every probe | single reads | ≥ 90 °C | ≥ 70 °C; TMU off |
| TOUCH | `hw_tp_start` (only when the boot menu would start it anyway) | always | menu.c: 150 ms power-up + bounded I2C | no answer | over 600 ms |
| MICROSD | card detect GPA1-5 | same | one read | — | — |

### Time budget on a healthy phone (estimates, not measured)

| Step | Time | New cost? |
|---|---|---|
| Bixby window | 400 ms + 2 × 345 KiB row copy | **new** |
| POST row save/restore | 2 × 4.5 MB uncached, 8 loads in flight: ~10-40 ms | **new** |
| RAM 1 MiB × 2 passes | ~5-15 ms | **new** |
| Timer, PMIC, TEMP, MICROSD | < 1 ms | new |
| Battery | ~1-3 ms (I2C at 400 kHz) | new |
| UFS + GPT + store + catalog | tens of ms | moved from `cat_load` |
| Touch | ~170-250 ms | moved from menu v7's start |
| Drawing 13 lines | a few ms | new |

So the POST takes about 0.3 s on screen, and the boot gets roughly **0.45-0.5 s longer** in
total: 400 ms of that is the Bixby window. To shorten the window, change `REC_WINDOW_MS` in
`recovery.h`. POST still catches a Bixby press after the window.

### Results

- **RAM**: `post_last()` returns `struct post_report` (`post.h`): per check the state, the value
  (battery %, °C, entries, MHz × 10), µs and the detail text. The menu can show it.
- **Log page** (0x91fff000; readable from Linux with `devmem 0x91fff000 + 8 * slot`):
  - slot 200: `"POST" << 32 | mode << 24 | n << 16 | fails << 8 | warns`
  - slot 201: total µs
  - slots 202..212: per check, `state << 56 | (value & 0xffffff) << 32 | µs`
  - States: 0 skip, 1 ok, 2 warn, 3 fail. The page is zeroed at every loader start.
- **pmsg ring** (TWRP: `/sys/fs/pstore/pmsg-ramoops-0`): `STARBITE: POST quick 312ms fail 0 warn 0`
  and one line for each failed or warned check.

## 5. Rescue mode

1. In the menu, **Starbite Rescue mode** looks for a rescue system: an `SBRESCUE` bundle in BOOT
   (CRCs checked), or the store's `rescue.initrd` with `rescue.image|linux.image` and
   `rescue.dtb|linux.dtb`.
   - If none is found: "NO RESCUE SYSTEM: <why>", and the menu stays.
   - If one is found: it writes the key `rescue` into the boot-next mailbox (0xFED17FE0, the key
     first, then the magic `SBNEXT2!` last) and resets with INFORM3 normal.
2. After S-Boot, the loader runs the Bixby window, reads the mailbox and clears it. It then starts
   the rescue system directly, with no POST, catalog or menu: from the bundle out of RAM, or from
   the store with `boot_linux`. The kernel command line gets `sb.rescue=1`.
   - Log slot 10 is 0x700 for the bundle and 0x701 for the store.
   - Holding Bixby during this boot cancels the request (recovery opens instead).
   - If the start fails, the recovery menu opens with the reason.
3. The ramdisk (`tools/rescue/init`) says "Connect a Starbite rescue USB disk". It waits for a
   filesystem labelled `STARBITE-RESCUE` (or `SB-RESCUE`: FAT32 and exFAT labels hold only 11
   characters) on USB (or microSD), mounts it read-only, checks `rescue.sha256` and runs
   `starbite-rescue/rescue.sh`. Power held 2 s reboots to the menu. USB host mode needs the
   store's `rescue.dtb` (docs/RESCUE-USB-HOST.md).
4. `reboot-to rescue` from any Starbite Linux does the same thing: the existing tool writes the
   same mailbox.

### Placeholder parts

- No rescue system is in the store yet. `rescue.initrd`, `rescue.dtb` and a matching
  `rescue.image` are built (2026-11-05, docs/RESCUE-USB-HOST.md "Deploy"); the `SBRESCUE` bundle
  tool has only been tested by a round-trip against a Python copy of the loader's parser.
- **USB host mode is written but has not run on the phone** (docs/RESCUE-USB-HOST.md): the
  rescue DTB makes the DWC3 a host by default, the MAX77705 charger gives 5 V (OTG boost, 900 mA)
  while its Type-C MCU reports a device attached. The daily DTB keeps the port a peripheral.
  Until host mode is proven, a microSD partition labelled `STARBITE-RESCUE` or `SB-RESCUE` is the
  working way in.
- The Starbite Installer, which should make the rescue disk, is item 7 of the plan.

## 6. RAM used

All in the loader's free XNU window (non-cacheable, unused until an OS is loaded):

| Address | Size | Use |
|---|---|---|
| 0x9C000000 | 16.3 MiB | recovery: the panel on entry |
| 0x9E000000 | 4.5 MiB | POST: the rows it draws over |
| 0x9E800000 | 345 KiB | the Bixby hint's row |
| 0xA0000000 | 1 MiB (64 MiB full) | POST RAM pattern |

EngineerMode's RAM test uses 0x92000000..0x93000000 and menu v5's screen copy 0x94000000..0x95043000,
so neither overlaps these.

## 7. Risks and what to check on the first boot

- **The 400 ms window and the POST pause run on every boot.** If a check is red on this phone
  for a harmless reason, every boot pauses 1.5 s until it is fixed. The battery bus is the most
  likely one: its bus code is a bounded copy of em.c's and has not run in this order.
- **The battery check sets up the CMGP I2C bus at every boot**: CMU_CMGP gates for USI_CMGP03,
  USI mode, GPM14/15 pinmux, and the controller reset. S-Boot uses the same bus, and EngineerMode
  does the same setup when you open its battery page. No MAX77705 register is written. No MIF/INT
  clock is touched.
- **Touch now starts in POST** rather than when menu v7 starts. The rail writes and checks are the
  same, only earlier. If touch fails in POST, menu v7 tries again (`hw_tp_start` retries while
  `tp_ok` is 0).
- **The intro capture**: POST and the hint put back exactly what they drew over. If the restore
  were wrong, v7's intro would show POST text in the captured splash. Check this in the first
  photo.
- **Writing the mailbox**: the recovery menu writes 32 bytes at 0xFED17FE0, the same mailbox and
  format as `reboot-to`. No flash is written anywhere in this feature.
- **Font**: fbtext has no lowercase. Catalog names are shown upper case, and characters it does
  not know (such as `·`) print as `?`.

First boot, three cases:
1. Normal boot: POST lines flash at the top, then the menu. Read slots 200..212 and check the
   pmsg line.
2. Hold Bixby: the recovery menu. Try Continue, Run POST (full) and Boot entry.
3. Rescue with no rescue system installed: "NO RESCUE SYSTEM: NO RESCUE BUNDLE IN BOOT, NO
   RESCUE.INITRD IN THE STORE", and the menu stays.
