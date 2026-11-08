/* SPDX-License-Identifier: GPL-2.0-only
 * Tiny drawing toolkit for EngineerMode: clipped rectangles and anti-aliased
 * text from the glyph atlas (tools/brand/em-font.py), straight to S-Boot's
 * framebuffer (MMU off). Text is blended over a known background colour, so
 * nothing is read back from the panel.
 */
#pragma once
#include <stdint.h>
#include "ui_glyphs.h"

#define UI_W 1440
#define UI_H 2960

/* Holo Dark (Android 4.x) palette, 0xAARRGGBB */
#define UI_BLACK 0xff000000U
#define UI_BG 0xff000000U
#define UI_BAR 0xff212121U          /* action bar + tab strip (menu.mjs EM.BAR) */
#define UI_HOLO 0xff33b5e5U
#define UI_HOLO_DIM 0xff1f6f8bU
#define UI_DIV 0xff2a2a2aU
#define UI_TEXT 0xffffffffU
#define UI_TEXT2 0xffa8a8a8U
#define UI_TEXT3 0xff6a6a6aU
#define UI_FOCUS 0xff0f3644U        /* holo blue 30 % over black */
#define UI_PRESS 0xff1a5a72U        /* holo blue 50 % over black */
#define UI_GREEN 0xff99cc00U
#define UI_RED 0xffff4444U
#define UI_ORANGE 0xffffbb33U
#define UI_PURPLE 0xffaa66ccU
#define UI_DIALOG 0xff282828U

/* fonts (em-font.py SPEC) */
#define F_BODY 0
#define F_SMALL 1
#define F_TAB 2
#define F_TITLE 3
#define F_MONO 4
#define F_BIG 5
#define F_STATUS 6
#define F_NAME 7        /* Outfit 300 64: boot menu v6 */
#define F_META 8        /* Outfit 300 40 */
#define F_CHIP 9        /* Outfit 500 28 */
#define F_HINT 10       /* Outfit 300 34 */

/* bg for ui_text / colour alpha for ui_round: blend with what the target already holds */
#define UI_BEHIND 0U

int ui_init(void);
/* Draw into buf (UI_W x UI_H, e.g. a write-back off-screen buffer) instead of the framebuffer
 * (0 = the framebuffer again). */
void ui_target(uint32_t *buf);
void ui_clip(int y0, int y1);
/* Rounded rectangle, anti-aliased corners; c's alpha (0xAA......) blends over the target. */
void ui_round(int x, int y, int w, int h, int r, uint32_t c);
void ui_fill(int x, int y, int w, int h, uint32_t c);
/* Menu sprite id (xnu_boot.c, "SBMENU01") at (x, y), alpha-blended over the target at fade/256:
 * tint 0 keeps the sprite's colours, else the sprite is a mask painted in tint (0xffRRGGBB).
 * (star_sprite_to copies sprites opaquely, alpha included: fine for the opaque header, black
 * squares around anything drawn over transparency.) 0 if there is no such sprite. */
int ui_sprite(unsigned id, int x, int y, unsigned fade, uint32_t tint);
/* Draw s with its baseline at y; returns the x after the last glyph. */
int ui_text(int font, int x, int y, const char *s, uint32_t fg, uint32_t bg);
int ui_text_r(int font, int right, int y, const char *s, uint32_t fg, uint32_t bg);
int ui_text_c(int font, int cx, int y, const char *s, uint32_t fg, uint32_t bg);
int ui_text_w(int font, const char *s);
int ui_ascent(int font);
int ui_line(int font);

/* Small string builder (no libc). */
struct ub {
    char b[256];
    unsigned n;
};
void ub_init(struct ub *u);
void ub_s(struct ub *u, const char *s);
void ub_c(struct ub *u, char c);
void ub_dec(struct ub *u, uint64_t v);
void ub_sdec(struct ub *u, int64_t v);
void ub_hex(struct ub *u, uint64_t v, unsigned digits);   /* "0x" + digits */
void ub_hexn(struct ub *u, uint64_t v, unsigned digits);  /* digits only */
void ub_dec2(struct ub *u, unsigned v);                   /* two digits, zero padded */
void ub_fix(struct ub *u, uint64_t milli, unsigned decimals);  /* 1234 -> "1.234" */
void ub_size(struct ub *u, uint64_t bytes);               /* "16 MiB", "2 GiB" */
unsigned ui_strlen(const char *s);
