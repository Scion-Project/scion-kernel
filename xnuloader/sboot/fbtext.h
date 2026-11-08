/* SPDX-License-Identifier: GPL-2.0-only
 * Based on ios-a5xelte shim/herolte/fbtext.h (wardevsa, herolte branch). */
#pragma once
#include <stdint.h>

/* S-Boot scanout buffer (uniLoader board-starlte.c / starlte_defconfig). */
#define STAR_FB_BASE 0xcc000000UL
#define STAR_FB_WIDTH 1440U
#define STAR_FB_HEIGHT 2960U

/* Let CPU writes reach the panel (DECON_F HW trigger), returns old value. */
uint32_t star_fb_enable(void);
void star_fb_restore(uint32_t before);
void star_fb_fill(uint32_t pixel);
void star_fb_row_base(unsigned row);
void star_fb_text(unsigned row, const char *s);
void star_fb_text_c(unsigned row, const char *s, uint32_t fg, uint32_t bg);
void star_fb_fill_rows(unsigned y0, unsigned y1, uint32_t pixel);
unsigned star_fb_row_y(unsigned row);
/* Two symbols for status lines (post.c, recovery.c): a check mark and a cross. */
#define FBT_CHECK "\x01"
#define FBT_CROSS "\x02"
/* Text rows are 60 px; 38 characters fit across the panel. */
#define STAR_FB_COLS 38U
void star_fb_hex(unsigned row, const char *label, uint64_t value);
/* While on, star_fb_text*, star_fb_hex and star_fb_fill_rows draw nothing: menu v7 mutes the
 * loader's status lines while its launch card is on the panel; fatal() unmutes. */
void star_fb_mute(int on);
