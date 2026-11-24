# Fast charging on the Galaxy S9: the owner's test

What to do with a Samsung fast wall charger (Adaptive Fast Charging, 9 V; for example the EP-TA20 that came with the S9,
"Adaptive Fast Charging" printed on it) and the phone's cable. This is the one step a computer's port cannot test.
The facts and the rules are in [CHARGING.md](CHARGING.md).

## Before you start

- The phone runs a kernel with the charger guard and the fast-charge path on: `sweetchg=2 sweetafc=1` in its boot line
  (phone 2 was left so on 24 November; on another phone flash such an image first, see "The images").
- Battery under 90 % (under 98 % is required; lower shows the fast charge better). Between 18 and 35 C: indoors, not in
  the sun, not under a pillow.
- Stay with the phone for the 15 minutes. Put it on a hard surface.

## Steps

1. Plug the phone into the Mac mini (its USB console), unlocked, screen on. On the Mac mini:
   `~/s9/agents/scion-phoneB/con.sh "sysctl kern.hakios.chg_afc"` must log `1`, and
   `con.sh "sysctl kern.hakios.chg_type"` must log `2` (a computer's charging port) or `1`. Write down the battery %.
2. Unplug from the Mac. Plug the cable into the fast wall charger, then into the wall.
3. Watch the phone for 15 minutes. Touch its back now and then.
4. Unplug from the wall charger and plug back into the Mac mini. On the Mac mini:
   `~/s9/agents/scion-phoneB/con.sh log` (the kernel's whole log again), then from its log file
   (`cat ~/s9/agents/scion-phoneB/logs/current`):
   `grep -a "STARLTE CHG" $(cat ~/s9/agents/scion-phoneB/logs/current)`

## What you should see

On the phone: the battery's lightning bolt within a few seconds; the percentage going up about 1 % every 1-1.5
minutes below 80 % (at 5 V it is about half that). The phone gets warm, not hot.

In the log, in this order:

```
STARLTE CHG: ... a wall charger at 5 V (DCP) ...            the charger seen as a DCP
STARLTE CHG: input limit: 0xc0 ... -> 0x13, read back 0x13  500 mA while asking
STARLTE CHG: AFC: 9 V request sent (0x46): ok
STARLTE CHG: AFC: answer opcode 0x21 result 0 VBADC 6; USBC_STATUS1 VBADC 6
STARLTE CHG: AFC: 9 V; the input and charge current go to 1650/2150 mA at the next second
STARLTE CHG: input limit: 0xc0 0x13 -> 0x41, read back 0x41    1650 mA
STARLTE CHG: charge current: 0xb9 0x6a -> 0x6b, read back 0x6b     2150 mA
STARLTE CHG: charging allowed; a fast charger at 9 V (AFC); on, ...
```

The console after plugging back into the Mac: `con.sh "sysctl kern.hakios.chg_afc_good"` is 1 or more,
`chg_afc_drops` says how many times it went back to 5 V (0 is expected unless the battery reached 98 % or warmed past 39 C).

If the charger is not an AFC charger, or the answer is not good: `AFC: no 9 V; 5 V limits, not asked again on this
cable`, and it charges at 5 V, 1800 mA in, 2100 mA charge. That is a safe result, not a failure of the phone.

## Stop at once (unplug from the wall) if

- the phone is too hot to hold, or the battery or the charger's thermistor in the log reads 41 C or more and it is still
  at 9 V (it should have gone back to 5 V by itself at 39 C, and stopped charging at 41 C);
- the phone or the charger smells, swells, clicks or the screen flickers;
- the percentage goes down while plugged in for more than 2 minutes;
- the log shows `DIFFERS`, `A READ FAILED`, `a fault` or `45 C or more` (the guard has already switched charging off;
  unplug and send the log).

## The images

On the Mac mini, in `~/s9/agents/fastcharge-1124/img/` (the fork's charger-guard commit, kernel k4):

| image | boot line | use |
|---|---|---|
| `xnu-boot-fa.img` | phone 2's usual line + `sweetafc=1` (the guard comes from the kernel's default line) | this test; what phone 2 was left on |
| `xnu-boot-fg.img` | phone 2's usual line | the guard only, no fast charge |
| `~/s9/agents/smooth-1123/img/xnu-boot-L6btm.img` | phone 2's usual line, the kernel before this work | charging stops about 70 s after boot |

To put one on phone 2: `~/s9/agents/scion-phoneB/run.sh IMAGE "SWEET LIMITS: SpringBoard" 420` (it goes to TWRP,
writes BOOT only, reads it back, boots).
