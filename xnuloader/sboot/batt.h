/* SPDX-License-Identifier: GPL-2.0-only
 * The battery for the boot menus: MAX77705 fuel gauge and charger status on HSI2C 0x14360000
 * (USI_CMGP03), read only. A copy of EngineerMode's reader (em.c, build/em-research-battery.md)
 * with bounded waits: a dead bus costs a few ms once, then the reader gives up for the boot.
 *
 * Read only: no MAX77705 register is ever written (S-Boot arms the charger watchdog, CNFG_00
 * WDTEN; nothing here feeds it, so charging can pause while the menu sits, CHG_DTLS 0xB, until
 * the OS starts and re-initialises the charger). The clear-on-read interrupt registers are never
 * read. Compiled general-regs-only (no FP), callable from menu7 (FP) and the other menus.
 */
#pragma once
#include <stdint.h>

struct batt_info {
    uint8_t valid;          /* 0: unknown (no answer, battery absent, model not loaded) */
    uint8_t pct;            /* RepSOC 0..100 */
    uint8_t plugged;        /* a cable is in: CHG_INT_OK CHGIN_OK (USB) or WCIN_OK (wireless) */
    uint8_t charging;       /* CHG_DTLS 0..2: pre-charge, fast CC, fast CV */
    uint16_t mv;            /* VCELL, mV (0 if that read failed) */
    int16_t ma;             /* Current, mA, + = into the battery (0 if that read failed) */
};

/* Bring the bus up and probe the PMIC (once; later calls return the first answer).
 * 1 the MAX77705 answered, 0 not (every batt_* call then returns "unknown" at once). */
int batt_init(void);

/* One reading (RepSOC, VCELL, Current, CHG_INT_OK, CHG_DETAILS_01): about 0.5 ms on a working
 * bus. 1 and *b valid, or 0 and b->valid = 0 (b is always written). */
int batt_read(struct batt_info *b);

/* Single values: -1 unknown. */
int batt_percent(void);
int batt_mv(void);
int batt_plugged(void);
