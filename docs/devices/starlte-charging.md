# Galaxy S9 charging under iOS 16.5: the charger guard (`max77705_chg=`)

26 November 2026. Off by default. Not yet run on a phone: everything below is written from Samsung's numbers and checked
on a computer (`tools/max77705-charge-test.sh`), not seen on an S9.

The iOS 16.5 kernel's MAX77705 driver (`drivers/power/max77705/hakios.max77705.c`, `max77705=1`) only reads the charger,
apart from the watchdog kick (`max77705_wdt=1`). Whatever S-Boot left in the charger stays: on a computer's charging port
that is Samsung's CDP group (1500 mA in, 1500 mA charge, measured on phone 2), and whatever S-Boot picks on a wall charger.
With the guard on, the kernel sets the charger to Samsung's own S9 limits for the charger type at 5 V, and applies
Samsung's temperature rules, which nothing in the chip knows (the battery's thermistor is on the AP's ADC).

The rules are the ones the iOS 27 kernel's guard follows: [docs/ios27/CHARGING.md](../ios27/CHARGING.md) has every fact and
where it comes from. The code that asks them is `drivers/power/max77705/hakios.max77705_guard.h`; the rules are
`hakios.max77705_charge.h`.

## Boot arguments

| argument | what happens |
|---|---|
| (none), `max77705_chg=0` | as before: nothing written but the watchdog kick with `max77705_wdt=1` |
| `max77705_chg=1` | read only. Every 5 s: the MUIC's charger type, the charger's state and settings, the battery's thermistor. The log says what the guard would write (`max77705: chg: would: ...`) whenever its decision changes |
| `max77705_chg=2` | the guard: as 1, and it writes (below) |
| `max77705_siop=0` | with the guard: the in-use limit off (it is on by default) |

S9 (starlte) only. On the S9+ or the Note9 the guard says it is for the S9 and does nothing (their limits differ).

## What is written (`max77705_chg=2`), and why those values

Charger at I2C 0x69 on CMGP USI03's HSI2C. Each write only when the value differs, only the bits named, read back:

| register | bits | value | source |
|---|---|---|---|
| CNFG_09 0xC0 | [6:0] input limit, (code+1) x 25 mA | SDP 475 mA (code 0x12), CDP 1500 (0x3B), DCP 1800 (0x47), unknown 500 (0x13) | `cable-info` current groups of `exynos9810-starlte_eur_open_26.dts` (USB group_12, CDP group_3, TA default, TIMEOUT group_1) |
| CNFG_02 0xB9 | [5:0] charge current, code x 50 mA | SDP 550 (0x0B), CDP 1500 (0x1E), DCP 2100 (0x2A), unknown 500 (0x0A); 700 (0x0E) at 15.0 C and down | the same groups; `swelling_low_temp_current` 700 |
| CNFG_00 0xB7 | bit 0, charger on/off | off: battery 41.0 C and up (on at 39.0 C), 5.0 C and down (on at 10.0 C), no thermistor reading, full (on at 4280 mV or 98 %), session time over (SDP 36000 s, CDP/DCP 18000 s) | `swelling_*`, `recharge_condition_*`, `*_charging_total_time` |
| CNFG_06 0xBD | [1:0] WDTCLR = 01 | the charger's watchdog kicked every poll while CNFG_00's WDTEN is on | Samsung's `max77705_chg_set_wdtmr_kick()` |

On top, and only ever lower:

- **In use (SIOP)**: while the panel is lit, or its level is not known yet, 1200 mA in and 1000 mA charge (S9 DTS:
  `battery,siop_input_limit_current = <1200>`, `siop_charging_limit_current = <1000>`). Samsung applies these when its
  thermal service lowers `siop_level`; here the panel's brightness (`hakios_panel_level()`, 0 = dark) stands in for "in use".
- **A computer on the USB link** (the gadget configured by a host): at most the CDP's 1500/1500 mA, whatever the MUIC called
  the port (a Mac's port was seen detected as a wall charger, and a Mac cut its port when a phone drew 1.7-2.0 A).

Latched off (charging off until the cable is pulled): battery 45.0 C or more, a chip fault (VBUS over-voltage, no or dead
battery, the timer's fault, thermal shutdown), the float voltage reading above 4350 mV, or a limit that does not read back
as written.

Never written: the float voltage (CNFG_04), the timers (CNFG_01), CHGPROT (if CNFG_06 [3:2] is not 11 the guard writes
nothing and says so), CNFG_00's mode bits (bit 0 only, and only while the mode is 4 or 5: never in OTG), any other
charger register, the gauge, the MUIC (no opcode: no AFC, the D+/D- switch untouched), the PMIC, anything non-volatile.
Nothing is written without VBUS or after a failed read.

AFC (9 V) is not in this change. It will be a separate boot argument, off by default, after 5 V has been seen on a phone.

## Test plan (an S9, iOS 16.5 image)

You need: the image built from this branch, the USB console or the loader's log, a Samsung wall charger (any; 5 V is
all that is asked), the battery under 90 %, room temperature.

1. Boot with your usual line plus `max77705_chg=1` (read only). In the log:
   `max77705: chg: the charger guard, read only, ...` at step 5, then one line per change, for example
   `max77705: chg: a wall charger at 5 V (DCP); charging allowed; on, input 1200 mA, charge 1000 mA, in use (SIOP); battery 27.3 C, ...`
   and `max77705: chg: would: input limit (CNFG_09): 0x.. -> 0x2f`. Check that the battery temperature is plausible
   (not `NOT READ`), and that the type matches the charger (computer port: SDP or CDP; wall charger: DCP).
2. Boot with `max77705_chg=2`. On the wall charger with the screen off for a few minutes, expect
   `input limit (CNFG_09): ... -> 0x47, read back 0x47` (1800 mA) and `charge current (CNFG_02): ... -> 0x2a, read back 0x2a`
   (2100 mA); with the screen on, 0x2f / 0x14 (1200 / 1000 mA). The battery should rise visibly faster than before at the
   same charger. Every 5 minutes: `max77705: chg: N writes, 0 failed, 0 read back differently, 0 refused (locked)`.
3. On the Mac's port: `a computer on the link` once the USB console is configured; never more than 1500/1500 mA.
4. Stop and unplug if: the phone gets hot to hold, the log says `DIFFERS`, `FAILED`, `45 C or more`, `a fault`, or the
   percentage falls while plugged in for more than a couple of minutes. Send the log.

To revert: boot without `max77705_chg=` (or with `max77705_chg=0`). The registers written are volatile: the next boot's
S-Boot sets its own again, and pulling the cable lets the chip's detection start over.

## Credits

The SIOP and "a computer on the link" limits are ideas from Jesvi-iOS (github.com/jesvi-eulah/Jesvi-iOS, iOS 16.5 on the
Galaxy S20, by jesvi-eulah; MIT outside its kernel files): `docs/devices/x1slte/charging-samsung.md` (fc07d1b), and the
charging commits 3a4db07, fb882cf and a57b321 of `xnu/files/bsd/dev/redwood_max77705.c` (APSL-2.0). No code of it is copied;
the values here are the S9's, from Samsung's S9 device tree. See NOTICE.
