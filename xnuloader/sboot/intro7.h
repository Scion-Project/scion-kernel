/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7's intro (intro7.c): S-Boot's splash -> Liquid Glass -> the menu's first frame, the
 * concept's introAt(t) / finishIntro() (docs/concepts/ios27-boot.html), 2350 ms. Section 4.3 of
 * docs/MENU7-PORT.md.
 *
 * S-Boot has shown logo.jpg (Samsung's own splash, from UP_PARAM) since power on; the loader
 * leaves it on the panel while it bakes the wallpaper. intro7_capture then copies what is on the
 * panel (its non-black row bands, I7_CAPT) and the intro starts from exactly that picture: the
 * captured bands are over-layer images (SB7_S_CAPT0.., sb7_put) that fade out (the logo block
 * rising, as the concept's "Galaxy S9"), except Samsung's own wordmark, which moves into the
 * concept wordmark's place and fades as the island comes out of it (no glass wordmark: the only
 * SAMSUNG on screen is the real one); the wallpaper, island and menu are the concept's. Only when nothing can be
 * captured (a black panel, too much picture, the area not free) does the intro draw the concept's
 * own splash (SAMSUNG, "Galaxy S9", "Powered by starbite") as before. Every frame is a pure
 * function of t and the captured picture: intro7_frame(t) builds the menu (m7_build with the
 * intro's struct m7_xf), the wallpaper's uniforms (struct g7_wall), the glass wordmark (struct
 * g7_word) and the splash items, and glass draws it. Any key or tap skips to the end; Bixby held
 * (700 ms) skips and opens Lab.
 *
 * Compiled with FP/SIMD. Frozen interface: a change goes through the integrator.
 */
#pragma once
#include <stdint.h>
#include "glass.h"
#include "menu7.h"

/* intro7_run's results */
#define I7_DONE 0                   /* ran to the end: the menu's first frame is on the panel */
#define I7_SKIPPED 1                /* a key or a tap ended it (finishIntro(true)) */
#define I7_LAB 2                    /* Bixby held: the caller opens Lab after finishIntro(true) */

/* Run the intro at M7_FPS from t = 0 (real time: frames are dropped, never slowed down). Calls
 * m7_prepare_boot first and m7_finish_intro at the end. */
int intro7_run(void);
/* The intro's frame at t ms into f (no drawing; the preview harness renders state=intro&t=). */
void intro7_frame(struct g7_frame *f, float t);
/* Copy S-Boot's splash off the panel (G7_FB) into I7_CAPT, before anything is drawn on it.
 * intro7_run calls it first; the preview harness calls it after putting a splash on its fake
 * panel. Returns the bands captured (the native splash path), or 0: the concept's splash. */
int intro7_capture(void);
/* Unregister the captured bands (SB7_S_CAPT0 ..) and forget them; intro7_run calls it on its way
 * out, so I7_CAPT holds nothing anyone reads after the intro. Their decoded copies stay in
 * G7_CACHE until glass resets it (unreachable: sb7_get no longer finds the ids). */
void intro7_release(void);

/* ---- the captured splash (docs/MENU7-PORT.md section 3) ---- */

/* 0x83800000 .. 0x84000000, write-back, below G7_CANVAS: used only when, at run time, the BOOT
 * Image (_start + image_size), the payload appended to it (star_info rd_start .. rd_end, via
 * m7_rd_range) and the menu ramdisk's SB7 tail (star_font_blob) all lie outside it. Only the
 * intro reads it (glass decodes each band into G7_CACHE at its first frame); intro7_release
 * unregisters it when the intro ends. */
#define I7_CAPT       (0x83800000UL + G7_HOSTOFF)
#define I7_CAPT_SIZE  0x00800000UL
#define I7_CAPT_BANDS 8             /* row bands (images SB7_S_CAPT0 ..); closer ones merge */
#define I7_CAPT_GAP   16            /* black rows that still join two bands: 2 * I7_CAPT_PAD, so two
                                     * padded boxes never have to split a JPEG block's faint values */
#define I7_CAPT_PAD   8             /* each band box grows by this (one JPEG block), copied raw */
#define I7_CAPT_MAX_PX (384U * 1024U)   /* 1.5 MiB of G7_CACHE (3 MiB) at most */
/* the timeline from the captured picture: bands above I7_NAT_SPLIT (the logo block) fade over
 * I7_GS9_FADE and rise -36 eo(t / I7_GS9_RISE), as #gs9; the ones below ("Powered by ...") fade
 * over I7_POW_FADE; the glass wordmark (word.ta) stays 0. */
#define I7_NAT_SPLIT  1973          /* 2 / 3 of the panel */
/* Samsung's wordmark in the captured picture: the band across panel row I7_NAT_SAM_ROW (logo.jpg
 * rows 48..127 at S-Boot's y 1144), at most I7_NAT_SAM_MAX_H rows. It is the only SAMSUNG shown
 * (no glass wordmark from a capture): still until I7_NAT_MOVE0 ("Galaxy S9" gone), then eased
 * into the concept wordmark's box (I7_NAT_SAM_W wide, aspect kept, centred on I7_CAP_X, I7_CAP_Y)
 * by I7_MELT0, then it shrinks by I7_CAP_W1 / I7_CAP_W0 and fades over the melt (to I7_ISLAND0). */
#define I7_NAT_SAM_ROW 1232
#define I7_NAT_SAM_MAX_H 160
#define I7_NAT_MOVE0  I7_GS9_FADE
#define I7_NAT_SAM_W  900.0f
/* the wordmark band is a line of text: at least I7_NAT_WORD_ASPECT times as wide as it is tall */
#define I7_NAT_WORD_ASPECT 3U
/* Starbite's own splash ("One mark", docs/concepts/splash/HANDOFF.md): when no wordmark crosses
 * I7_NAT_SAM_ROW, the band across I7_NAT_WORD_ROW (the capsule's centre) is the word "starbite".
 * It is already where the capsule is born: it keeps its size (k = 1) and settles on I7_CAP_X. */
#define I7_NAT_WORD_ROW 1380
/* The splash's star: a band I7_STAR_MIN .. I7_STAR_MAX square (sides within a quarter of each
 * other), blue (2 sum B >= 3 sum R and sum B >= sum G: not green, cyan or teal), star-shaped (its
 * alpha fills at most 1 / I7_STAR_FILL_DIV of the box: One mark's 0.23, a solid tile 1.0) and in
 * the logo block (its centroid above I7_NAT_SPLIT). Its R is clamped to I7_STAR_R_MIN ..
 * I7_STAR_R_MAX (the wallpaper star's final 54). It hands over in place: it fades over I7_STAR_FADE with no
 * rise while the wallpaper's star lights at its centre (the alpha centroid) and its R (the
 * alpha area over I7_STAR_AREA = the area of WALL_FS's core |x|^.62 + |y|^.62 <= 1 with its
 * smoothstep(.86, 1) edge, at R = 1: 4 G(1 + 1 / .62)^2 / G(1 + 2 / .62) * .7945), and flies
 * home through I7_FLY_STAR_C. Without one the star starts at M7_STAR0 and flies through I7_FLY_C. */
#define I7_STAR_MIN 24U
#define I7_STAR_MAX 128U
#define I7_STAR_AREA 0.825350f
#define I7_FLY_STAR_CX 1350.0f
#define I7_FLY_STAR_CY 1180.0f
#define I7_STAR_FILL_DIV 2U
#define I7_STAR_R_MIN 8.0f
#define I7_STAR_R_MAX 54.0f
/* A capture that failed (I7_CAPT_NOT_FREE, I7_CAPT_TOO_BIG, I7_CAPT_TABLE_FULL: S-Boot drew
 * something, it was not copied) does not draw the concept's SAMSUNG splash over it: the panel is
 * cleared and the wallpaper star lights at One mark's star (the approved splash) and flies through
 * I7_FLY_STAR_C, a fade from black. Only a black panel (I7_CAPT_BLACK) gets the concept's splash. */
#define I7_ONE_STAR_X 961.74f
#define I7_ONE_STAR_Y 1259.0f
#define I7_ONE_STAR_R 27.0f
/* intro7_capture's result in the log page (0x91fff000, uncached), slots I7_LOG_CAPT ..:
 *   [116] px << 32 | I7_LOG_TAG << 16 | why << 8 | bands   (px: the bands' total area)
 *   [117] the capture's duration in us (both passes)
 * why: I7_CAPT_NOT_FREE 0, I7_CAPT_BLACK 1, I7_CAPT_TOO_BIG 2 (px > I7_CAPT_MAX_PX),
 *      I7_CAPT_TABLE_FULL 3 (sb7_put), I7_CAPT_OK 0xff (the native path ran) */
#define I7_LOG        0x91fff000UL
#define I7_LOG_CAPT   116
#define I7_LOG_TAG    0xca97U
#define I7_CAPT_NOT_FREE   0U
#define I7_CAPT_BLACK      1U
#define I7_CAPT_TOO_BIG    2U
#define I7_CAPT_TABLE_FULL 3U
#define I7_CAPT_OK         0xffU

/* ---- the timeline (ms), the concept's introAt; e = m7_ease, eo = m7_eout, c01 = clamp to 0..1 ---- */

#define I7_MS 2350                  /* INTRO_MS */
/* splash, over layer (above the menu): */
#define I7_SAM_FADE 220             /* SAMSUNG (white) opacity 1 - t / 220 */
#define I7_GS9_FADE 260             /* "Galaxy S9" opacity 1 - t / 260, rises -36 * eo(t / 420) px */
#define I7_GS9_RISE 420
#define I7_POW_FADE 200             /* "Powered by", "starbite" 1 - t / 200 */
#define I7_STAR_FADE 120            /* the wordmark's star 1 - t / 120 (it becomes the wallpaper's star) */
/* the wallpaper's star flies a quadratic path star0 -> C -> STAR: */
#define I7_FLY0 120                 /* e = ease((t - 120) / 950) */
#define I7_FLY_MS 950
#define I7_FLY_CX 1390.0f           /* control point (from M7_STAR0; a captured star: I7_FLY_STAR_C) */
#define I7_FLY_CY 1520.0f
#define I7_BLOOM0 950               /* bloom = sin(pi * c01((t - 950) / 600)) */
#define I7_BLOOM_MS 600
/* wall: si = c01(t / 120) * (1 + .45 bloom); sr = lerp(star0 r, 54, e((t - 180) / 900));
 *       sw = .72 * e((t - 250) / 700); rr = t < 600 ? 0 : 3900 * eo((t - 600) / 1000); rf 800; amb 1 */
#define I7_SI_RAMP 120
#define I7_SR0 180
#define I7_SR_MS 900
#define I7_SW0 250
#define I7_SW_MS 700
#define I7_REV0 600
#define I7_REV_MS 1000
#define I7_REV_R 3900.0f
#define I7_REV_FEATHER 800.0f
/* dim = lerp(1, .8, e((t - 1300) / 700)) */
#define I7_DIM0 1300
#define I7_DIM_MS 700
/* glass wordmark: tf = 1 - e((t - 220) / 520); tm = e((t - 700) / 350); ta = t < 1050;
 * cap = (720, 1380, lerp(1180, 940, tm), lerp(170, 136, tm)) */
#define I7_DRAIN0 220
#define I7_DRAIN_MS 520
#define I7_MELT0 700
#define I7_MELT_MS 350
#define I7_CAP_X 720.0f
#define I7_CAP_Y 1380.0f
#define I7_CAP_W0 1180.0f
#define I7_CAP_W1 940.0f
#define I7_CAP_H0 170.0f
#define I7_CAP_H1 136.0f
/* the capsule becomes the island at 1050: pill glass alpha 1, scale 1,
 * pill_dy = 1136 * (1 - spA((t - 1050) / 1000 s, .45, .80)); pillIn opacity c01((t - 1250) / 300) */
#define I7_ISLAND0 1050
#define I7_ISLAND_DY 1136.0f
#define I7_ISLAND_IN0 1250
#define I7_ISLAND_IN_MS 300
/* menu: ta = spA((t - 1330) / 1000, .60, .86): brand, title dy 90 (1 - ta), opacity c01(1.3 ta);
 * subtitle dy 110 (1 - spA((t - 1380) / 1000, .6, .86)), opacity c01((t - 1380) / 400);
 * foot c01((t - 1700) / 400); status c01((t - 1350) / 400);
 * section i: s = spA((t - 1400 - 55 i) / 1000, .55, .80): dy 320 (1 - s), scale .96 + .04 s,
 *            alpha c01(1.6 s); dock: s = spA((t - 1550) / 1000, .55, .80): dy 340 (1 - s),
 *            glass alpha c01(1.6 s); lens alpha c01((t - 1850) / 300) */
#define I7_TITLE0 1330
#define I7_TITLE_DY 90.0f
#define I7_SUB0 1380
#define I7_SUB_DY 110.0f
#define I7_FOOT0 1700
#define I7_STATUS0 1350
#define I7_CARDS0 1400
#define I7_CARD_STAGGER 55
#define I7_CARD_DY 320.0f
#define I7_DOCK0 1550
#define I7_DOCK_DY 340.0f
#define I7_LENS0 1850
#define I7_FADE_MS 400
