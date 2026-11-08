/* SPDX-License-Identifier: GPL-2.0-only
 * Exynos 9810 USB 2.0 PHY (USBCON 0x11100000) for the loader's DWC3 (usb_phy.c). */
#pragma once
#include <stdint.h>
#include "fastboot.h"

/* Opens the three USB clock gates, releases the PMU isolation and runs the vendor CAL enable
 * sequence for this PHY revision. 0, or -1 when USBCON does not read back (*why). */
int usb_phy_start(const char **why);
/* Clocks, PMU and USBCON registers as text, before (as the loader found them) and after. */
void usb_phy_diag(struct tb *t);
/* the D+/D- line state the PHY sees: "SE0" (nothing, or reset), "J" (idle), "K", "SE1" */
const char *usb_phy_line(void);
