# Charging the Galaxy S9 under iOS 27 (and fast charging)

24 November 2026, phone 2 (23edfb2021027ece). What charges the S9, what Samsung's kernel does about it, what iOS 27
did before this change, and what the charger guard (`sweetchg=`) and the fast-charge path (`sweetafc=`, off by
default) do now. The owner's test plan for a fast wall charger is [CHARGING-TEST.md](CHARGING-TEST.md).

The facts come from Samsung's kernel for this phone (Scion-Project/android_kernel_samsung_exynos9810, lineage-24.0:
`drivers/battery_v2/max77705_charger.c`, `sec_battery.c`, `drivers/muic/max77705-muic.c`, `max77705-muic-afc.c`,
`include/linux/muic/max77705-muic.h`, `include/linux/ccic/max77705.h`, and the device tree
`exynos9810-starlte_eur_open_26.dts`). That code is GPL. It was read for numbers and register meanings only. Nothing of
it is copied here or in the code.

"Measured" means read from phone 2. "Inferred" means taken from the source or from what was measured, but not seen.

## The chips

| part | what it does | where |
|---|---|---|
| MAX77705, charger block | the switching charger: input current limit, charge current, float voltage, top-off, the charger's own watchdog and safety timer | I2C 0x69 on CMGP USI03's HSI2C (0x14360000) |
| MAX77705, fuel gauge | ModelGauge: state of charge (RepSOC), cell voltage, current, capacity. It learns the battery by itself | I2C 0x36, same bus |
| MAX77705, MUIC / USB-C controller | VBUS detection, BC 1.2 charger type, USB-C CC and PD, the D+/D- switch, the AFC 9 V handshake. A firmware that takes opcodes | I2C 0x25, same bus |
| MAX77705, top (PMIC part) | the chip's ID, interrupts, haptic block | I2C 0x66, same bus |
| AP's ADC (CMGP, 0x14230000) | the thermistors: channel 4 the battery, channel 3 the charger, channel 1 the USB port | not the MAX77705 |
| S2MPS18/S2MPS19 | the SoC's main PMICs (CPU, GPU, memory rails). Not part of charging | SPMI/ACPM |
| wireless charger (mfc-charger) | Qi pad charging. Not used or touched here | its own I2C |

The battery is 3000 mAh nominal (`battery_full_capacity`); the gauge's design value is 2967 mAh (`fuelgauge,capacity`).
Phone 2's gauge says 3104 mAh full now (measured).

## How Samsung picks the charger type

1. The MUIC sees VBUS and runs BC 1.2 detection by itself. The result is `BC_STATUS` (0x08): bit 7 VBUS, [1:0] ChgTyp
   (1 a plain USB port, SDP; 2 a computer's charging port, CDP; 3 a dedicated charger, DCP), bit 2 DCD time-out,
   [5:3] PrChgTyp (Samsung 2 A, Apple 0.5/1/2 A/12 W, 3 A DCP).
2. The USB-C side (CC pins, Rp current, PD) is read as well. A USB-C source with Rp 1.5/3 A, or PD, becomes cable
   type `PDIC` and is charged by its power (15 W at most here, `pd_charging_charge_power`).
3. The MUIC driver turns this into an "attached device" (TA, USB, CDP, Unofficial TA, DCD time-out, AFC 5 V/9 V ...).
   The battery driver turns that into a cable type and takes that type's current group from `cable-info`.
4. A TA (DCP) is a candidate for AFC.

Measured on phone 2 at the Mac mini's port: `BC_STATUS` 0x82 (VBUS, CDP, no special type), USB-C CC0 0xb1. In TWRP,
Samsung's own kernel called it `PDIC` (cable 19) and set 3000 mA in, 2150 mA charge (15 W, the port's USB-C Rp 3 A).

## The AFC (Samsung Adaptive Fast Charging) 9 V handshake

Inferred from Samsung's MUIC driver; not seen (no fast charger here):

1. A DCP is seen. The MUIC driver waits 500 ms ("Maxim's request").
2. The battery driver lowers the input limit to `pre_afc_input_current`, 500 mA.
3. The MUIC driver sends opcode 0x21 (`COMMAND_AFC_RESULT_READ`) with one byte: 0x46 for 9 V (0x08 for 5 V). Opcodes
   go to register 0x21 (`OPCODE_WRITE`), then 0x41 (`OPCODE_WRITE_END`).
4. The MUIC firmware talks to the charger on D- (the AFC ping protocol). The answer comes back at 0x51
   (`OPCODE_READ`): the opcode, a result, the VBUS band (VBADC), then up to 8 bytes the charger sent.
5. Result 0 with VBADC 5, 6 or 7 (7.5-10.5 V) means the charger is at 9 V. Other results: 1 no CHGIN, 2 not a
   high-voltage DCP, 3 not a DCP, 4 no ping answer, 5 unsupported byte (retry with one the charger offered), 6/7 VBUS
   did not change, 8-10 a short on the cable, 11 not Rp 56 k, 16-18 protocol errors. Samsung retries 3 times, then
   resets AFC (`BC_CTRL2` 0x13) if VBUS is still high.
6. The battery driver then sets the 9 V limits: 1650 mA in, 2150 mA charge.
7. Samsung asks for 5 V again when the battery is full, when its thermal level asks for less, and in store mode. With
   the charger thermistor at 46 C or more it holds 9 V at 1000 mA in, 1250 mA charge.
8. For a TA the MUIC's D+/D- switch is left open (`COM_OPEN`), not connected to the AP's USB.

## Samsung's limits (exynos9810-starlte_eur_open_26.dts)

| charger | cable type | input limit | charge current | session time |
|---|---|---|---|---|
| plain USB port (SDP) | USB (4), current_group_12 | 475 mA | 550 mA | 36000 s (`usb_charging_total_time`) |
| computer's charging port (CDP) | USB_CDP (5), current_group_3 | 1500 mA | 1500 mA | 18000 s |
| wall charger at 5 V (DCP) | TA (3), the default | 1800 mA | 2100 mA | 18000 s (`normal_charging_total_time`) |
| fast charger at 9 V | 9V_TA (6), current_group_4 | 1650 mA | 2150 mA | 10800 s (`hv_charging_total_time`) |
| DCD time-out, unknown | TIMEOUT (30), current_group_1 | 500 mA | 500 mA | |
| USB-C PD/Rp | PDIC (19) | by the source's power, 3000 mA at most | 2150 mA | |

- Float voltage 4350 mV (`chg_float_voltage`; CNFG_04 0x13, measured). Top-off 300 then 150 mA.
- Full: charging switched off (CNFG_00 0x14, measured in TWRP's log). Recharge under 4280 mV or under 98 %.
- Temperatures (battery thermistor, tenths of a degree in the tree):
  - 50.0 C and up, or 0.0 C and down: health "overheat"/"cold", charging stops. Back at 45.0 C / 5.0 C.
  - 41.0 C and up, or 5.0 C and down ("swelling"): charging stops. Below 4000 mV it recharges at a 4150 mV float,
    1090 mA (hot) or 700 mA (cold). It ends at 39.0 C and below, 10.0 C and above.
  - 15.0 C and down: 700 mA at most. Ends at 20.0 C.
  - Charger thermistor 46.0 C and up with 9 V: 1000 mA in, 1250 mA charge. Ends at 44.0 C.
- Timers: Samsung's driver turns the charger's hardware fast-charge timer OFF (CNFG_01 FCHGTIME 0) and its
  hardware restart off. It counts time itself (`expired_time` 10800 s, the total times above).
- The charger's watchdog (CNFG_00 bit 4) is turned on by Samsung's driver and kicked at every poll, every 10 s
  (CNFG_06 WDTCLR = 01). If it is not kicked, the charger stops (CHG_DTLS 0xB).

## What hardware enforces alone, and what needs software

Inferred from the register meanings and from what Samsung's driver does:

- **Hardware alone:** VBUS over-voltage (CHGIN OVLO), the input current limit once set, the charge current once set, the
  float voltage once set, the charger's own die temperature (regulation, then thermal shutdown, CHG_DTLS 0xA),
  battery-to-system over-current (B2SOVRC), battery over-voltage, the watchdog once enabled, top-off and done.
- **Software only:** the battery's temperature (the thermistor is on the AP's ADC, not the charger, so nothing in the
  chip knows it), the charger type's limits (the chip takes whatever input limit it is given), the safety timer
  (Samsung turns the hardware timer off), recharging after full (hardware restart is off), keeping the watchdog fed,
  and everything about AFC.

## What iOS 27 did before this change (measured on phone 2)

The iOS 27 kernel (Scion-Project/xnu, `starlte_muic.c` and `SweetBattery.cpp`, `sweetfg=1`) only read the gauge and
`BC_STATUS`. It never wrote the charger. So:

1. **Charging stopped about 70 s after every boot.** S-Boot leaves the charger's watchdog on (CNFG_00 0x15) and kicks it
   last. Nobody kicked it in iOS. At 15:35:01, 69 s after the kernel started, DETAILS_01 went from 0x33 (top-off) to
   0x3b (CHG_DTLS 0xB, watchdog expired), and stayed there for the whole window. iOS showed "cable in, not charging".
   With a full battery this hardly shows. With a half-empty one the phone would not have charged in iOS at all.
2. **No battery temperature at all.** iOS was given a fixed 25.0 C. Nothing stopped charging when hot or cold.
3. **No safety timer.** S-Boot and TWRP leave the hardware timer off (CNFG_01 0xb8, measured).
4. **"Charging" guessed from the current** (more than 20 mA into the battery), not from the charger.
5. **After a full charge in TWRP the charger is left off** (Samsung writes CNFG_00 0x14 at full). S-Boot turned it on
   again at the next boot here (CNFG_00 0x15 in iOS, measured), so this one did not reach iOS.

What S-Boot leaves (measured in iOS, before anything was written): CNFG_00 0x15, CNFG_01 0xb8, CNFG_02 0x5e (1500 mA),
CNFG_03 0x99, CNFG_04 0x13 (4350 mV), CNFG_06 0x0c (CHGPROT unlocked), CNFG_09 0x3b (1500 mA), CNFG_12 0x60. That is
Samsung's CDP current group, so S-Boot detects the type the same way.

## What the kernel does now

`bsd/dev/starlte_charger.c` in the fork, with the rules in `hakios.max77705_charge.h` (scion-kernel
`drivers/power/max77705/`, a copy in the fork's `bsd/dev/`; host tests `tools/max77705-charge-test.sh`, in CI).

| boot argument | what happens |
|---|---|
| `sweetchg=0` | nothing new. SweetBattery as before. (`sweetchg=2` is in the kernel's built-in default line since this change: `sweetchg=0` on the loader's line turns it off) |
| `sweetchg=1` | read only. Every second: the charger, MUIC, gauge and both thermistors. iOS gets the real battery temperature, "charging" from the charger's own state, "full" from top-off/done, and adapter details (type, volts, amps). The log says what the guard would do ("would: ..."). Register dump at start and every 5 minutes |
| `sweetchg=2` | the guard. As 1, and it writes the charger (volatile registers only, each read back): watchdog kicked every 10 s and re-armed if it expired; charging on or off by the temperature bands, the session time, full and recharge; the input limit and charge current set to Samsung's for the type, or less |
| `sweetafc=1` | with `sweetchg=2`: the fast-charge path, OFF by default (below) |
| `sweetchg_drain=MIN` | a test: the first MIN minutes (30 at most) with the input at 100 mA and charging off, so the phone runs on its battery. Stops at 50 % or 40 C |

The guard's rules (all from Samsung's numbers above; stricter where they differ):

- Charger type from `BC_STATUS` only: SDP, CDP, DCP, or unknown (DCD time-out, Apple and other special chargers, ChgTyp
  0). USB-C Rp and PD are not used: a USB-C source is charged at its BC 1.2 type's limits, which are lower.
- Off at 41.0 C and up until 39.0 C and down; off at 5.0 C and down until 10.0 C and up; 700 mA at most at 15.0 C and
  down until 20.0 C. No swelling recharge (stricter than Samsung).
- 45.0 C or more, the float voltage reading above 4350 mV, VBUS over-voltage, no or dead battery, the timer's fault,
  thermal shutdown, or a read-back that differs: charging off and latched off until the cable is pulled.
- No temperature reading: charging off (not latched).
- Session time per type (table above): then off until the cable is pulled.
- Full when the charger says done; on again under 4280 mV or 98 %.
- Never written: the float voltage, the timers, CNFG_01/03/04/05/07/08/10-12, the gauge, the PMIC, anything
  non-volatile.

### The fast-charge path (`sweetafc=1`, off by default)

9 V is asked for only when all of these hold:

- the MUIC says DCP (ChgTyp 3 with VBUS, no special type), for at least a second;
- the battery thermistor is above 15.0 C and at most 39.0 C, the charger thermistor is under 44.0 C, the battery is
  under 98 %, the chip reports no fault, VBUS reads 5 V;
- it has not failed or been dropped before on this cable.

Then: the input limit goes to 500 mA, the D+/D- switch is opened (as Samsung does for a TA), and the 9 V request is
sent. 9 V counts only if the answer is result 0 with VBADC 5-7 and `USBC_STATUS1` also reads 7.5-10.5 V. Only then
the limits go to 1650 mA in, 2150 mA charge.

Every second while at 9 V: if the battery leaves 15-39 C, the charger thermistor reaches 44 C, VBUS leaves 7.5-10.5 V,
the battery reaches 98 %, a fault shows, or a register reads back differently, the input goes to 500 mA, 5 V is asked
for, and the standard DCP limits (1800/2100 mA at 5 V) apply once VBUS reads 5 V. While VBUS is above 5.5 V outside a 9 V
session, the input stays at 500 mA. No new 9 V request on that cable.

On a computer port (SDP or CDP) and on unknown chargers it never asks.

## Measured on phone 2 (computer port: the Mac mini)

The minute-by-minute record is the run's NOTES (`~/s9/agents/fastcharge-1124/NOTES.md` on the Mac mini). Summary:

- **The port.** `BC_STATUS` 0x82: VBUS, ChgTyp 2 = CDP, no special type, no DCD time-out. VBUS band 2 (4.5-5.5 V).
  The guard calls it "a computer's charging port (CDP)" (`kern.hakios.chg_type` = 2) and sets Samsung's CDP limits,
  1500 mA in, 1500 mA charge. S-Boot had already set the same (no write needed).
- **Read-only window** (`sweetchg=1`, 15:33-15:43): the watchdog expiry above (charging stopped 69 s after the kernel
  started and never came back). Battery thermistor 25.8-29.2 C (TWRP read 25.6 C just before, through Samsung's own
  driver: the same reading); charger thermistor 26-32 C.
- **Guard window** (`sweetchg=2 sweetafc=1 sweetchg_drain=20`, 15:44-16:09): watchdog kicked every 10 s, DETAILS_01
  never 0x3b again. 20 minutes on the battery (input 100 mA, charging off; light load): 100 % to 90 %, -0.9 A at the
  start, battery 29.2-31.0 C. At the end the guard set the CDP limits back (CNFG_09 0x03 -> 0x3b) and charging on
  (CNFG_00 0x14 -> 0x15), each read back the same. The charger went to fast charge (DETAILS_01 0x31), then CV
  (0x32). iOS showed the charging bolt. No 9 V request: the port is a CDP (the AFC log lines: none).
- **End state** (the kernel's default line with `sweetchg=2`, plus `sweetafc=1`): the guard on from boot, CDP,
  1500/1500 mA, charging in CV at 92 %, +373-419 mA, battery 30.1-31.1 C, charger 33.7-35.8 C, 14 watchdog kicks in
  the first two minutes, 0 writes beyond the kicks, 0 read-backs that differed, 0 9 V requests.

Register writes made on phone 2 in this work, all volatile charger registers, each read back the same:
CNFG_06 WDTCLR kick (0x0c -> 0x0d), CNFG_00 bit 0 (0x15 <-> 0x14), CNFG_09 input limit (0x3b <-> 0x03 for the drain
test). Not written: CHGPROT (it read unlocked), CNFG_02 (S-Boot's 1500 mA was already Samsung's CDP value), anything of
the MUIC (no DCP), the gauge, the PMIC, any NVM or OTP.

Not tested (needs a fast wall charger): the DCP limits, the 9 V request and its answer, the 9 V limits, the drops back to
5 V. [CHARGING-TEST.md](CHARGING-TEST.md) is that test.
