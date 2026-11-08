/* SPDX-License-Identifier: GPL-2.0-only
 * MAX77705 on HSI2C 0x14360000 for the loader's USB: the Type-C MCU's D+/D- switch and the fuel
 * gauge (max77705_muic.c). */
#pragma once
#include <stdint.h>
#include "fastboot.h"

/* Route D+/D- from the connector to the AP (CONTROL1 = COM_USB, read back, four tries).
 * 1 written, 0 the MCU did not answer. */
int muic_route_usb(void);
/* Call every 500 ms: closes the switch again 0, 0.5, 1.5, 3 and 6 s after VBUS comes in (the
 * MCU's own detection may reopen it). Returns 2 when VBUS has just come back (a replug: the
 * caller restarts the link), 1 while present, 0 without, -1 on a bus error. */
int muic_poll(void);
/* Fuel gauge (0x36): RepSOC in percent and VCELL in mV; 1 if read. */
int muic_battery(unsigned *mv, unsigned *pct);
void muic_diag(struct tb *t);
