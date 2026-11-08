/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 assets: the "SB7ASSET" container (docs/MENU7-PORT.md section 4.4).
 *
 * Written by tools/brand/menu7-assets.*, appended to the BOOT ramdisk after the v6 sprites
 * ("SBMENU01") and EngineerMode's glyph atlas ("SBFONT01"), at a 4 KiB aligned address
 * (S-Boot loads the ramdisk at 0x89000000, so the offset is 4 KiB aligned in the ramdisk too).
 * Read in place (write-back RAM) by sb7.c. Little endian, every offset from the container start,
 * every section 16-byte aligned, no pointers.
 *
 *   sb7_head                          32 bytes
 *   sb7_dir[count]                    sorted by (type, id): sb7_get is a binary search
 *   sections                          FONT, IMAGE, TILE, TILEMAP, HEIGHT, INFO
 *
 * Frozen interface: builders implement against it; a change goes through the integrator.
 */
#pragma once
#include <stdint.h>

#define SB7_VERSION 1U
#define SB7_ALIGN 4096U             /* where the container may start (ramdisk offset) */

struct sb7_head {
    char magic[8];                  /* "SB7ASSET" */
    uint32_t version;               /* SB7_VERSION */
    uint32_t count;                 /* directory entries */
    uint32_t size;                  /* the whole container in bytes, this header included */
    uint32_t dir_crc;               /* CRC-32 (IEEE 802.3, as zlib.crc32) of the directory */
    uint32_t data_crc;              /* CRC-32 of bytes [32 + 16 * count, size) */
    uint32_t flags;                 /* 0 */
};

struct sb7_dir {
    uint32_t type, id;              /* SB7_T_*, the section's id (font, image, tile ids below) */
    uint32_t off, size;             /* from the container start; off % 16 == 0 */
};

#define SB7_T_FONT    1U            /* struct sb7_font + glyphs + kerning + A8 bitmaps */
#define SB7_T_IMAGE   2U            /* struct sb7_image + pixels (raw or RLE) */
#define SB7_T_TILE    3U            /* struct sb7_tile: an OS tile (id = tile id) */
#define SB7_T_TILEMAP 4U            /* id 0: struct sb7_tilemap_ent[]: catalog key -> tile id */
#define SB7_T_HEIGHT  5U            /* id 0: the SAMSUNG height field, struct sb7_height + A8 */
#define SB7_T_INFO    6U            /* id 0: NUL-terminated ASCII: tool version, font sources + sha256 */

/* ---- fonts -------------------------------------------------------------------------------- */

/*
 * One section per (family, weight, size), rasterised by FreeType at exactly that size with the
 * concept's features (font-feature-settings "cv11", "ss01" on the whole screen; "tnum" on the
 * countdown digits), kerning, grayscale anti-aliasing, no hinting (as Chrome on macOS). Inter is
 * Google Fonts' Inter[opsz,wght] at opsz = min(px, 32) (CSS font-optical-sizing: auto).
 * Letter-spacing is not baked in: the text call applies it (CSS: after every character).
 */
struct sb7_font {
    uint16_t px;                    /* size in panel px */
    uint16_t weight;                /* 300 .. 800 */
    uint16_t family;                /* SB7_FAM_* */
    uint16_t nglyph;
    int32_t asc64, desc64, gap64;   /* hhea ascender, -descender (> 0), line gap, 1/64 px */
    uint16_t nkern;
    uint16_t flags;                 /* SB7_FF_* */
    uint32_t glyph_off;             /* sb7_glyph[nglyph], sorted by cp; from this struct's start */
    uint32_t kern_off;              /* sb7_kern[nkern], sorted by (left, right) */
    uint32_t bits_off, bits_size;   /* the A8 bitmaps */
};
struct sb7_glyph {
    uint32_t cp;                    /* Unicode code point (after cv11/ss01: the alternate's shape) */
    int32_t adv64;                  /* advance, 1/64 px, letter-spacing excluded */
    uint16_t w, h;                  /* bitmap size (0 x 0 for a space) */
    int16_t bx, by;                 /* bitmap top-left from the pen on the baseline; by < 0 above */
    uint32_t off;                   /* w * h bytes of coverage, row-major, from bits_off */
};
struct sb7_kern {
    uint16_t left, right;           /* glyph indices into this font's table */
    int32_t adj64;                  /* added to left's advance, 1/64 px */
};

#define SB7_FAM_INTER_TIGHT 1U
#define SB7_FAM_INTER       2U
#define SB7_FAM_MONO        3U      /* JetBrains Mono */

#define SB7_FF_TNUM 1U              /* digits rasterised with "tnum" */
#define SB7_FF_SUBSET 2U            /* only the glyphs of fixed strings (see the charset column) */

/*
 * Font ids (= the section id). Column: family weight px, where the concept uses it, charset.
 * Charset "text": U+0020..U+007E, U+00A0, U+00B0 ° U+00B1 ± U+00B5 µ U+00B7 · U+00D7 ×
 * U+2013 – U+2014 — U+2018 ‘ U+2019 ’ U+201C “ U+201D ” U+2026 … U+2190..U+2193 ← ↑ → ↓
 * U+2248 ≈ U+25A0 ■ U+25B2 ▲ U+25BC ▼ U+2713 ✓ (the catalog's characters + the concept's).
 */
#define SB7_F_IT700_142  1U   /* Inter Tight 700 142  large title "Choose a System"        subset */
#define SB7_F_IT700_128  2U   /* Inter Tight 700 128  provider names                         subset: A-Z a-z */
#define SB7_F_IT700_100  3U   /* Inter Tight 700 100  error title                            text */
#define SB7_F_IT700_92   4U   /* Inter Tight 700  92  sheet name, launch name                text */
#define SB7_F_IT700_70   5U   /* Inter Tight 700  70  alert title                            text */
#define SB7_F_IT700_40   6U   /* Inter Tight 700  40  countdown digit (tnum)                 subset: 0-9 */
#define SB7_F_IT600_66   7U   /* Inter Tight 600  66  system row name                        text */
#define SB7_F_IT600_54   8U   /* Inter Tight 600  54  end screen title                       text */
#define SB7_F_IT600_46   9U   /* Inter Tight 600  46  status bar, island, notice title       text */
#define SB7_F_IT500_60  10U   /* Inter Tight 500  60  splash "Galaxy S9"                     subset */
#define SB7_F_IT300_66  11U   /* Inter Tight 300  66  splash wordmark "starbite"             subset */
#define SB7_F_IT300_56  12U   /* Inter Tight 300  56  menu wordmark "starbite"               subset */
#define SB7_F_IN500_58  13U   /* Inter 500 58  options menu items                            text */
#define SB7_F_IN600_58  14U   /* Inter 600 58  sheet Start, error Try Again / Back           text */
#define SB7_F_IN600_56  15U   /* Inter 600 56  alert buttons                                 text */
#define SB7_F_IN600_46  16U   /* Inter 600 46  segmented control, error reason chip          text */
#define SB7_F_IN600_33  17U   /* Inter 600 33  tags, dock labels                             text */
#define SB7_F_IN400_50  18U   /* Inter 400 50  sheet rows, error message                     text */
#define SB7_F_IN400_48  19U   /* Inter 400 48  subtitle, alert message, launch subtitle      text */
#define SB7_F_IN400_46  20U   /* Inter 400 46  sheet values, sheet subtitle                  text */
#define SB7_F_IN400_44  21U   /* Inter 400 44  provider subtitle                             text */
#define SB7_F_IN400_42  22U   /* Inter 400 42  row detail, "Start Automatically" label       text */
#define SB7_F_IN400_40  23U   /* Inter 400 40  row status, launch stage, end subtitle        text */
#define SB7_F_IN400_38  24U   /* Inter 400 38  footer hints, launch hint                     text */
#define SB7_F_IN400_36  25U   /* Inter 400 36  island second line, notice second line        text */
#define SB7_F_IN400_31  26U   /* Inter 400 31  splash "Powered by"                           subset */
#define SB7_F_JB500_40  27U   /* JetBrains Mono 500 40  sheet mono values                    text */
#define SB7_F_JB500_36  28U   /* JetBrains Mono 500 36  error code line                      text */
#define SB7_F_JB500_34  29U   /* JetBrains Mono 500 34  launch detail, end screen mono       text */
#define SB7_F_COUNT     30U   /* ids are 1 .. SB7_F_COUNT - 1 */

/* ---- images ------------------------------------------------------------------------------- */

#define SB7_A8   1U                 /* coverage, painted in the item's colour */
#define SB7_ARGB 2U                 /* 0xAARRGGBB premultiplied (sRGB values) */
#define SB7_RAW  0U
#define SB7_RLE  1U

/*
 * RLE: a stream of little-endian u16 control words c. c < 0x8000: c literal pixels follow;
 * c >= 0x8000: one pixel follows, repeated c - 0x8000 times. A pixel is 1 byte (A8) or 4 bytes
 * (ARGB); the bytes after a control word are padded to a 2-byte boundary. Rows run on (a run may
 * cross a row end). Decoded once, at g7_init, into G7_CACHE.
 */
struct sb7_image {
    uint16_t w, h;                  /* bitmap size */
    uint8_t format, enc;            /* SB7_A8 / SB7_ARGB, SB7_RAW / SB7_RLE */
    uint16_t flags;                 /* 0 */
    uint16_t nx, ny, nw, nh;        /* the nominal box inside the bitmap: what the item's w, h
                                     * map to (an icon's 24-unit viewBox, a glyph's 96 px box;
                                     * the rest is drop shadow or anti-aliasing margin) */
    uint32_t data_size;             /* bytes after this struct */
};

/* Line icons (SF-style, the concept's <symbol id="i-*"> on a 24 grid, round caps and joins),
 * rendered at the size and stroke width the concept draws them. A8 unless noted. */
#define SB7_I_STARF_40     1U   /* i-starf 40: subtitle */
#define SB7_I_STARF_32     2U   /* i-starf 32: Default tag */
#define SB7_I_STARF_54     3U   /* i-starf 54: notice ("Is the Default") */
#define SB7_I_STAR_58      4U   /* i-star 58 (outline): "Set as Default" */
#define SB7_I_CHECK_58     5U   /* i-check 58: "Default System" */
#define SB7_I_CHECK_54     6U   /* i-check 54: island "Ready" */
#define SB7_I_PLAY_58      7U   /* i-play 58 (filled): "Start" */
#define SB7_I_INFO_58      8U   /* i-info 58: "Details" */
#define SB7_I_INFO_54      9U   /* i-info 54: notices */
#define SB7_I_TIMER_46    10U   /* i-timer 46: "Start Automatically" */
#define SB7_I_TIMER_54    11U   /* i-timer 54: notices, island "Auto-start Off" */
#define SB7_I_FLASK_54    12U   /* i-flask 54: notices */
#define SB7_I_CLOCK_36    13U   /* i-clock 36: "Last Used" */
#define SB7_I_LIFE_74     14U   /* i-life 74, stroke 1.9: dock Recovery */
#define SB7_I_DOWN_74     15U   /* i-down 74, stroke 1.9: dock Download */
#define SB7_I_FLASK_74    16U   /* i-flask 74, stroke 1.9: dock Lab */
#define SB7_I_POWER_84D   17U   /* i-power 84, stroke 2.1: dock Power */
#define SB7_I_POWER_84    18U   /* i-power 84, stroke 2: alert icon */
#define SB7_I_LIFE_84     19U   /* i-life 84: alert icon */
#define SB7_I_DOWN_84     20U   /* i-down 84: alert icon */
#define SB7_I_X_54        21U   /* i-x 54, stroke 2.4: sheet close */
#define SB7_I_WARN_150    22U   /* i-warn 150: ARGB (#ffd60a body, #1a1206 mark): error icon */
#define SB7_I_BAT_84      23U   /* i-bat 84 x 40: ARGB white with its .45 parts: status bar */
#define SB7_I_GSTAR_56    24U   /* g-star at .42 em of 56 (23.52 px): menu wordmark star */
#define SB7_I_GSTAR_66    25U   /* g-star at .42 em of 66 (27.72 px): splash wordmark star */
#define SB7_I_BOLT_30     26U   /* charging bolt 18 x 30 (Starbite-drawn): A8, in the battery */
#define SB7_I_COUNT       27U

/* Provider tile glyphs: the concept's g-apple, g-win, g-andr, g-tux, g-star, g-flask (masks
 * included), white, 96 px nominal box, with the tile's drop-shadow(0 4px 10px rgba(0,0,0,.25))
 * baked around it: ARGB. */
#define SB7_P_APPLE      64U
#define SB7_P_MICROSOFT  65U
#define SB7_P_GOOGLE     66U
#define SB7_P_LINUX      67U
#define SB7_P_STARBITE   68U
#define SB7_P_LAB        69U
#define SB7_P_OTHER      70U        /* not in the concept: a white ellipsis in a ring */

/* Splash */
#define SB7_S_SAMSUNG    80U        /* A8 1300 x 220: drawSam(k = 1), the 900 x 120 wordmark centred */
/* 96 .. 103: not in the container. The splash S-Boot left on the panel, captured at run time by
 * intro7_capture: one ARGB (premultiplied) RAW image per row band, put in with sb7_put. */
#define SB7_S_CAPT0      96U

/* 256 .. 4095: tile glyph layers (ARGB), referenced only by sb7_tile.glyph[] */
#define SB7_G_FIRST     256U

/* ---- OS tiles ----------------------------------------------------------------------------- */

/*
 * An OS tile is drawn by glass at run time from this record (squircle n = 5, the two-stop
 * gradient from (0, 0) to (.75, 1) of its box, the glyph layer, the sheen white .28 -> 0 at .45
 * -> black .14, a 1.1-unit white rim at .24 centred on the outline), at 92 (island), 124 (rows),
 * 220 (sheet) and 380 (launch). The glyph layer is everything the concept's TG() draws, already
 * coloured (some glyphs use the tile's c2 or "dark" ink), at each of the four sizes.
 */
#define SB7_TILE_92  0U
#define SB7_TILE_124 1U
#define SB7_TILE_220 2U
#define SB7_TILE_380 3U
struct sb7_tile {
    uint32_t c1, c2;                /* 0x00RRGGBB */
    uint16_t glyph[4];              /* image ids (SB7_G_FIRST ..) per SB7_TILE_*, 0 = none */
    uint16_t flags;                 /* SB7_TF_DARK: light tile, dark ink (the concept's 4th field) */
    uint16_t pad;
};
#define SB7_TF_DARK 1U

/* Tile ids 1 .. 255 are the assets tool's (tools/brand/menu7-assets: one per concept tile and
 * per catalog entry); the C code only reaches them through the tile map. */
struct sb7_tilemap_ent {
    char key[24];                   /* catalog key; "" = the default for `category` */
    uint16_t tile;
    uint8_t category;               /* CAT_* (catalog.h), used when key is "" */
    uint8_t flags;                  /* SB7_TM_PREFIX: key matches any key starting with it */
};
#define SB7_TM_PREFIX 1U

/* ---- the SAMSUNG height field (intro only) ------------------------------------------------ */

/* The concept's hm canvas: drawSam(650 x 110, k = .5), alpha box-blurred r 3 twice (separable,
 * edge clamped), white; sampled bilinearly over the panel rect (fx, fy, fw, fh). */
struct sb7_height {
    uint16_t w, h;                  /* 650 x 110 */
    uint16_t fx, fy, fw, fh;        /* 70, 1270, 1300, 220 */
    uint32_t pad;                   /* then w * h A8 bytes */
};

/* ---- the reader (sb7.c) ------------------------------------------------------------------- */

/* Find and check the container in [start, end) (the BOOT ramdisk: star_font_blob's range
 * rounded down to 4 KiB): both CRCs (the A55's CRC32 instructions). 1 found and valid. */
int sb7_open(uintptr_t start, uintptr_t end);
/* The section (type, id): its address and size, or 0. */
const void *sb7_get(uint32_t type, uint32_t id, uint32_t *size);
/* The container's range once opened (for the preview harness and Lab). */
uintptr_t sb7_base(uint32_t *size);
/* Tile id for a catalog entry: exact key, then prefixes (longest first), then the category's
 * default; 0 if none. */
unsigned sb7_tile_for(const char *key, unsigned category);
/* A section made at run time (in RAM the caller owns, laid out as the container's): sb7_get finds
 * it before the container. p = 0 removes (type, id). 0 when the table (SB7_RT_MAX) is full. */
#define SB7_RT_MAX 8U
int sb7_put(uint32_t type, uint32_t id, const void *p, uint32_t size);

/* sb7.c also defines memcpy, memset and memmove (no libc in the loader): GCC may emit calls to
 * them for struct copies in code compiled with FP/SIMD (glass.c, menu7.c, intro7.c). */
