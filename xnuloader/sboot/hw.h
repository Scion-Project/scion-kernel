/* SPDX-License-Identifier: GPL-2.0-only
 * Hardware helpers shared by the boot menu (menu.c), EngineerMode (em.c) and
 * the loader (xnu_boot.c). MMU off, one CPU, S-Boot's state.
 */
#pragma once
#include <stdint.h>
#include "menu.h"

/* Keys (ALIVE GPIOs, active low): see menu.c. */
#define K_UP 1U
#define K_DOWN 2U
#define K_BIXBY 4U
#define K_POWER 8U

/* PMU boot-reason values (vendor kernel). */
#define INFORM_NORMAL 0x12345670U
#define INFORM_DOWNLOAD 0x12345671U
#define INFORM_RECOVERY 0x12345674U
#define PMU_BASE 0x14060000UL

uint64_t hw_ticks(void);
uint64_t hw_freq(void);
void hw_wait_us(uint64_t us);
void hw_wait_ms(unsigned ms);
unsigned hw_keys(void);
void hw_reboot(uint32_t inform3);
void hw_power_off(void);       /* returns only if the phone stayed on */

/* S2MPS18 through ACPM (type 0 TOP, 1 PMIC, 2 RTC). */
int hw_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
int hw_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
/* ACPM firmware build string (24 chars + NUL), 0 if the framework is absent. */
int hw_acpm_build(char out[25]);

/* Touch (S6SY761). hw_tp_event: finger 0 only; *st 1 down, 2 move, 3 up. */
int hw_tp_ok(void);
void hw_tp_start(void);
int hw_tp_event(int *st, int *x, int *y);
int hw_tp_read(uint8_t reg, uint8_t *data, unsigned len);
void hw_tp_max(unsigned *mx, unsigned *my);
void hw_tp_clear(void);

/* iOS boot options (boot-args of the payload, edited in place). */
struct star_opt {
    unsigned cpus;
    int rw, verbose, touch, touchpwr;   /* boot options */
    int multi, smpprobe;                /* experiments */
};
extern struct star_opt opt;
void hw_opt_load(const char *cmdline);
void hw_opt_apply(char *cmdline, unsigned len);
/* Touch rails on now (LDO35 1.8 V, LDO43 3.0 V) with the kernel's checks:
 * 1 written, 0 refused (unexpected voltage), -1 ACPM failure. */
int hw_touch_rails(uint8_t *l35, uint8_t *l43);
/* microSD rails for Linux: LDO31 2.8 V, LDO2 3.3 V (same return values). */
int hw_sd_rails(uint8_t *l31, uint8_t *l2);

/* xnu_boot.c */
int star_sprite_at(unsigned id, int x, int y);
int star_font_blob(uint64_t *start, uint64_t *end);
/* A bundle appended to BOOT ("SBUEFI!!", or the retired "SBLINUX!"): its address, or 0. */
uint64_t star_bundle(const struct star_info *in, int linux_not_uefi, uint64_t *size);

/* em.c: EngineerMode. Returns the catalog entry to start + 1 (catalog.h; BOOT's iOS with the
 * edited cmdline is star_ios_pick()) or 0 for the menu. */
int em_main(const struct star_info *in, char *cmdline, unsigned len);
/* em.c: the S2MPS18 RTC as Unix time (UTC); 0 if unreadable. */
int em_rtc_epoch(uint64_t *epoch);
