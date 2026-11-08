/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-sensors-r1/mag-r1/patches/0930-blazer-sensor-compass.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * MMC5616WA magnetometer (stock registry: mmc5616, i3c0, static I2C address 0x30) by the AP over a register bus.
 * Hardware-free: every access goes through struct mag_bus, so the host test runs this exact logic.
 * Register facts: MEMSIC MMC5616WA datasheet v1.7 (2025-12-22) pages 9-17, 27. Plan: research/ios26-sensors-r1/PLAN.md. */
#ifndef BLAZER_MAG_HW_H
#define BLAZER_MAG_HW_H
#include "blazer_hw_port.h"

#define MAG_ADDR                0x30u
#define MAG_REG_XOUT0           0x00u   /* 9 bytes: X/Y/Z [19:12], [11:4], then X/Y/Z [3:0] in the high nibbles of 0x06..0x08 */
#define MAG_REG_STATUS1         0x18u
#define MAG_REG_CTRL0           0x1bu
#define MAG_REG_PRODUCT_ID      0x39u
#define MAG_PRODUCT_ID          0x11u   /* reset value, page 15 */
#define MAG_CTRL0_TM_M          0x01u
#define MAG_CTRL0_DO_SET        0x08u
#define MAG_CTRL0_AUTO_SR       0x20u
#define MAG_MEASURE             (MAG_CTRL0_AUTO_SR | MAG_CTRL0_TM_M)    /* page 16's example: 0b00100001 */
#define MAG_NULL_COUNT          524288  /* 20-bit null field output, page 2 */
#define MAG_COUNTS_PER_GAUSS    16384   /* 20-bit, page 2 */
#define MAG_POWER_ON_US         20000u  /* power on time (Sw_reset text, page 13); tOp is 5 ms (page 27) */
#define MAG_SR_GAP_US           1000u   /* tSR, page 27 */
#define MAG_MEASURE_US          10000u  /* tTM at BW=00 is 6.6 ms (page 27): the caller's gap between mag_sample calls must be at least this */
#define MAG_STUCK_SAMPLES       50u     /* identical 9 bytes this many times in a row: not measuring. Noise alone is about 24 counts RMS at BW=00 */

struct mag_bus {
	int (*read)(void *ctx, uint32_t reg, uint8_t *out, uint32_t len);       /* 0, or the bus's own stop code */
	int (*write)(void *ctx, uint32_t reg, uint32_t value);
	void (*sleep_us)(void *ctx, uint32_t us);
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
	void *ctx;
};

enum mag_stop { MAG_OK = 0, MAG_STOP_BUS, MAG_STOP_ID, MAG_STOP_FLAT, MAG_STOP_STUCK, MAG_STOP_NOT_STARTED };

struct mag {
	struct mag_bus bus;
	uint32_t stop, stop_detail;
	uint8_t id, status_first;
	uint8_t last[9];
	uint32_t samples, same_run;
	bool started, pending;
};

const char *mag_stop_name(uint32_t stop);
/* ID must be 0x11; one SET, then the first measurement is started. Nothing but MAG_REG_CTRL0 is written, and only MAG_CTRL0_DO_SET or MAG_MEASURE */
int mag_start(struct mag *m, const struct mag_bus *bus);
/* Reads the measurement started by the previous call (or by mag_start), then starts the next one. Call no sooner than MAG_MEASURE_US after the last.
 * nt: nanotesla in iOS's device frame (x right, y up, z out of the screen), from the registry's transform -2 -1 -3; flip bits 0..2 undo the sign per output axis. */
bool mag_sample(struct mag *m, uint32_t flip, int32_t nt[3]);
/* Pure helpers, used by the test */
void mag_decode(const uint8_t b[9], int32_t counts[3]);         /* signed, 0 = null field */
void mag_to_ios_nt(const int32_t counts[3], uint32_t flip, int32_t nt[3]);
#endif
