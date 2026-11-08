/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Recovery (recovery.c, docs/STARBITE-RECOVERY.md): hold Bixby early in boot for a
 * plain text menu that needs nothing but the ALIVE key GPIOs, the generic timer and the
 * framebuffer (fbtext.c). Everything that touches storage or another peripheral runs only after
 * the user picks it.
 */
#pragma once
#include <stdint.h>
#include "menu.h"

/* How long the loader looks for a held Bixby key, right after the MMU and the panel trigger. */
#define REC_WINDOW_MS 400U

/* The Bixby window: a hint line at the top of the panel (put back afterwards), then up to ms of
 * polling GPA0 bit 6. 1 if Bixby was seen held (debounced, about 30 ms). Every wait is bounded
 * by an iteration count too, so a stopped timer cannot hang it. */
int rec_bixby_window(unsigned ms);
/* Bixby held right now (two reads 5 ms apart): POST polls it between its checks. */
int rec_bixby_now(void);

/* The rescue request: the boot-next mailbox (0xFED17FE0, menu.c boot_next) holding "SBNEXT2!" +
 * key "rescue" (written by the recovery menu, or by `reboot-to rescue` from Linux). 1 if it was
 * there, and then the mailbox is cleared at once, so a rescue that crashes is not retried; any
 * other key is left for the boot menu (menu.c boot_next). */
int rec_rescue_take(void);

/* What recovery_main asks the loader to do. */
#define REC_CONTINUE 0      /* the normal boot (POST, then the boot menu) */
#define REC_BOOT     1      /* start catalog entry *pick (Engineer Lite or "Boot entry...") */
#define REC_FASTBOOT 2      /* USB fastboot (fastboot_dev.c): run by recovery_loop in xnu_boot.c */

/* The recovery menu (blocks until the user leaves it; reboots and power off happen inside).
 * why: a line shown in red at the top (the reason recovery came up, or 0). */
int recovery_main(const char *why, int *pick);
/* The panel copy recovery_main took on entry is no longer valid (an OS load overwrote its
 * buffer): Continue then leaves a black panel instead of putting it back. */
void rec_panel_lost(void);

/* Panel rows y0..y1 (pixels) to buf and back: an exact copy, so menu v7's intro, which captures
 * S-Boot's splash from the panel, never sees what POST or the Bixby hint drew. */
void rec_rows_save(unsigned y0, unsigned y1, uintptr_t buf);
void rec_rows_restore(unsigned y0, unsigned y1, uintptr_t buf);
/* The text row base fbtext draws at now (star_fb_row_base's value). */
unsigned rec_row_base(void);

/* xnu_boot.c, for the recovery menu */
/* BOOT's payload, bundles and the menu assets found (nothing drawn), once: what Engineer needs. */
const struct star_info *star_boot_setup(char **cmdline, unsigned *len);
/* 1 if a rescue system exists (an "SBRESCUE" bundle in BOOT, else the store's rescue.initrd with
 * a kernel and DTB); otherwise *why says what is missing. May bring UFS up. */
int star_rescue_ready(const char **why);
/* xnu_boot.c: one line to the pmsg ring (TWRP's /sys/fs/pstore/pmsg-ramoops-0). */
void star_pmsg(const char *s);
