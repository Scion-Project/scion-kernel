// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot menu v7's intro: S-Boot's splash -> Liquid Glass -> the menu's first frame. intro7.h and
 * docs/MENU7-PORT.md section 4.3. This is the concept's introAt(t) (docs/concepts/ios27-boot.html,
 * reduced motion off) and the intro part of its frame() / key() / pointerdown, in C:
 *
 *   introAt(t)          intro7_frame(f, t): the menu's transforms (struct m7_xf) for m7_build,
 *                       then the wallpaper (f->wall), the glass wordmark (f->word), uDim (f->dim)
 *                       and the #splash items appended to the over layer
 *   frame() in 'intro'  intro7_run(): real time t, m7_step(dt), intro7_frame, g7_frame, 30 fps
 *   finishIntro         m7_finish_intro (menu7.c), then the menu's first frame
 *   key() / pointerdown any key or a tap skips; Bixby held ('lab') skips and asks for Lab
 *
 * Every number is the concept's (the constants in intro7.h). Compiled with FP/SIMD; no libm:
 * the springs' closed form, ease and eOut are menu7's (m7_spa, m7_ease, m7_eout); the bloom's
 * sine is a polynomial here.
 *
 * The first frame is S-Boot's own splash (logo.jpg as it is on the panel): intro7_capture copies
 * the panel's non-black row bands into I7_CAPT before anything is drawn, and the intro fades
 * them out of exactly that picture (deliberate difference D5, docs/MENU7-PORT.md). The concept's
 * splash items are drawn only when there is no capture.
 */
#include <stdint.h>
#include "intro7.h"
#include "glass.h"
#include "menu7.h"
#include "sb7.h"
#include "hw.h"

_Static_assert(I7_CAPT + I7_CAPT_SIZE <= G7_CANVAS, "I7_CAPT runs into G7_CANVAS");
_Static_assert(I7_CAPT >= 0x83800000UL + G7_HOSTOFF, "I7_CAPT below the BOOT Image's limit");
_Static_assert((uint64_t)I7_CAPT_MAX_PX * 4U + I7_CAPT_BANDS * 64U <= I7_CAPT_SIZE, "I7_CAPT too small");
_Static_assert((uint64_t)I7_CAPT_MAX_PX * 4U <= G7_CACHE_SIZE / 2U, "the captured splash must leave G7_CACHE room");

/* ---- the concept's helpers -------------------------------------------------------------------- */

static inline float c01(float x) { return m7_clamp(x, 0.0f, 1.0f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* sin(pi x) for x in [0, 1] (the bloom: Math.sin(Math.PI * clamp(...))). Folded to [0, .5], odd
 * Taylor series to the 13th power: |error| < 1e-9 on [0, pi / 2]. Exactly 0 at x = 0 and x = 1, so
 * the wallpaper is exactly the final one once the bloom is over. */
static float sinpi01(float x)
{
    if (x <= 0.0f || x >= 1.0f)
        return 0.0f;
    if (x > 0.5f)
        x = 1.0f - x;
    float a = 3.14159265358979f * x, a2 = a * a;
    float p = 1.0f / 6227020800.0f;                 /* 1 / 13! */
    p = p * a2 - 1.0f / 39916800.0f;                /* 1 / 11! */
    p = p * a2 + 1.0f / 362880.0f;                  /* 1 / 9!  */
    p = p * a2 - 1.0f / 5040.0f;                    /* 1 / 7!  */
    p = p * a2 + 1.0f / 120.0f;
    p = p * a2 - 1.0f / 6.0f;
    p = p * a2 + 1.0f;
    return a * p;
}

/* spA((t - t0) / 1000, r, z) with t in ms (the concept passes seconds) */
static inline float spa_ms(float t, float t0, float r, float z) { return m7_spa((t - t0) * 0.001f, r, z); }

/* ---- the splash (#splash: #samCv, #gs9, #pow > #powBy, .sbw > #powW + #spStar) ------------------ */

/* Item keys: 'i' (intro) in the top byte, unique in the over layer (menu7 uses other prefixes). */
#define I7_KEY(n) (((uint32_t)'i' << 24) | 0x5000U | (n))

static const char s_gs9[] = "Galaxy S9";
static const char s_powby[] = "Powered by";
static const char s_starbite[] = "starbite";

/* letter-spacing (em) of the splash's lines: #gs9 .005em, #powBy .02em, .sbw -.01em */
#define I7_GS9_TRACK 0.005f
#define I7_POWBY_TRACK 0.02f
#define I7_SBW_TRACK (-0.01f)
/* baselines (docs/concepts/boot-geometry/intro-0.json): #gs9 top 1528, font 500 60px/1 -> 1580;
 * #powBy 2518; #powW 2598 */
#define I7_GS9_BASE 1580.0f
#define I7_POWBY_BASE 2518.0f
#define I7_SBW_BASE 2598.0f
/* #samCv: left 70, top 1270, 1300 x 220 (SB7_S_SAMSUNG is drawSam(k = 1) at that size) */
#define I7_SAM_X 70.0f
#define I7_SAM_Y 1270.0f
#define I7_SAM_W 1300.0f
#define I7_SAM_H 220.0f

/* Advances of the three lines (text-align: center centres the advance, letter-spacing after the
 * last character included, as CSS does): measured once. */
static float w_gs9, w_powby, w_sbw;
static int widths_ok;

static void measure(void)
{
    if (widths_ok)
        return;
    w_gs9 = g7_text_w(SB7_F_IT500_60, s_gs9, sizeof s_gs9 - 1, I7_GS9_TRACK);
    w_powby = g7_text_w(SB7_F_IN400_31, s_powby, sizeof s_powby - 1, I7_POWBY_TRACK);
    w_sbw = g7_text_w(SB7_F_IT300_66, s_starbite, sizeof s_starbite - 1, I7_SBW_TRACK);
    widths_ok = 1;
}

static void add_text(struct g7_layer *l, uint32_t key, unsigned font, const char *s, unsigned n,
                     float x, float y, float track, uint32_t color, float a)
{
    if (a <= 0.0f)
        return;
    struct g7_item *it = g7_add_item(l, G7_TEXT);
    if (!it)
        return;
    it->key = key;
    it->a = a;
    it->color = color;
    it->u.text.s = s;
    it->u.text.n = (uint16_t)n;
    it->u.text.font = (uint16_t)font;
    it->u.text.x = x;
    it->u.text.y = y;
    it->u.text.track = track;
    it->u.text.scale = 1.0f;
    it->u.text.max_w = 0.0f;
    it->u.text.color2 = 0;
    it->u.text.gy0 = 0.0f;
    it->u.text.gy1 = 0.0f;
    it->u.text.gstop = 0.0f;
}

static void add_image(struct g7_layer *l, uint32_t key, unsigned id, float x, float y, float w,
                      float h, uint32_t color, float a)
{
    if (a <= 0.0f)
        return;
    struct g7_item *it = g7_add_item(l, G7_IMAGE);
    if (!it)
        return;
    it->key = key;
    it->a = a;
    it->color = color;
    it->u.image.id = (uint16_t)id;
    it->u.image.pad = 0;
    it->u.image.x = x;
    it->u.image.y = y;
    it->u.image.w = w;
    it->u.image.h = h;
    it->u.image.sat = 1.0f;
}

/* The #splash items at t, in paint order, after the menu's #over items. */
static void splash_items(struct g7_layer *l, float t)
{
    measure();
    /* $('samCv').style.opacity = 1 - clamp(t / 220) */
    add_image(l, I7_KEY(1), SB7_S_SAMSUNG, I7_SAM_X, I7_SAM_Y, I7_SAM_W, I7_SAM_H, M7_L1,
              1.0f - c01(t / I7_SAM_FADE));
    /* #gs9: opacity 1 - clamp(t / 260), translateY(-36 eOut(clamp(t / 420))) */
    add_text(l, I7_KEY(2), SB7_F_IT500_60, s_gs9, sizeof s_gs9 - 1, (G7_W - w_gs9) * 0.5f,
             I7_GS9_BASE - 36.0f * m7_eout(c01(t / I7_GS9_RISE)), I7_GS9_TRACK, M7_L1,
             1.0f - c01(t / I7_GS9_FADE));
    /* #powBy, #powW: 1 - clamp(t / 200) */
    float pa = 1.0f - c01(t / I7_POW_FADE);
    add_text(l, I7_KEY(3), SB7_F_IN400_31, s_powby, sizeof s_powby - 1, (G7_W - w_powby) * 0.5f,
             I7_POWBY_BASE, I7_POWBY_TRACK, M7_SPLASH_BY, pa);
    add_text(l, I7_KEY(4), SB7_F_IT300_66, s_starbite, sizeof s_starbite - 1, (G7_W - w_sbw) * 0.5f,
             I7_SBW_BASE, I7_SBW_TRACK, M7_L1, pa);
    /* #spStar (.sst: left 100% + .03em, top .02em, .42em square, fill var(--star)): 1 - t / 120.
     * Its box is exactly star0 (the wallpaper star starts where this one fades). */
    add_image(l, I7_KEY(5), SB7_I_GSTAR_66, M7_STAR0_X - M7_STAR0_R, M7_STAR0_Y - M7_STAR0_R,
              2.0f * M7_STAR0_R, 2.0f * M7_STAR0_R, M7_STAR, 1.0f - c01(t / I7_STAR_FADE));
}

/* ---- the captured splash ------------------------------------------------------------------------ */

#define I7_FB ((const volatile uint64_t *)(0xcc000000UL + G7_HOSTOFF))     /* glass.c's G7_FB */

struct capt_band {
    uint16_t y0, y1, x0, x1;        /* panel rows [y0, y1), columns [x0, x1) */
};
static struct capt_band CB[I7_CAPT_BANDS];
static int capt_n;                  /* bands captured; 0: the concept's splash */
static int capt_sam = -1;           /* the band that is the wordmark (SAMSUNG or "starbite"), or -1 */
static float sam_w;                 /* the width capt_sam glides to: I7_NAT_SAM_W, or its own (k = 1) */
static int capt_star = -1;          /* the band that is the splash's star (I7_STAR_*), or -1 */
static int capt_fail;               /* the capture failed (not a black panel): no concept splash */
static int star_own;                /* star_x, star_y, star_r hold the wallpaper star's start (a
                                     * captured star or, after a failure, I7_ONE_STAR); else M7_STAR0 */
static float star_x, star_y, star_r;    /* its alpha centroid and R: the wallpaper star's start */
static uint16_t row_x0[G7_H], row_x1[G7_H];     /* pass 1: each row's non-black columns */

#ifndef M7_PREVIEW
extern char _start[];               /* xnu_boot.S: the Image header (image_size at + 16) */
#endif

/* I7_CAPT is free: the BOOT Image ends below it (_start + image_size: with a payload appended,
 * that is boot_rd_end too), the payload appended to it (star_info's rd_start .. rd_end, from
 * boot_rd_*) does not overlap it, and neither does the SB7 part of the menu ramdisk (S-Boot loads
 * that at 0x89000000; star_font_blob gives its SBFONT01 tail, which ends where the ramdisk ends). */
#ifdef M7_PREVIEW
extern int h7_capt_busy;            /* h7: H7_CAPT_BUSY=1 plays I7_CAPT_NOT_FREE */
#endif
static int capt_free(void)
{
    uint64_t s, e;
#ifdef M7_PREVIEW
    if (h7_capt_busy)
        return 0;
#endif
#ifndef M7_PREVIEW
    uint64_t isz = *(const volatile uint64_t *)(uintptr_t)(_start + 16);
    if ((uintptr_t)_start + isz > I7_CAPT)
        return 0;
#endif
    if (m7_rd_range(&s, &e) && e > s && s < I7_CAPT + I7_CAPT_SIZE && e > I7_CAPT)
        return 0;
    if (star_font_blob(&s, &e) && s < I7_CAPT + I7_CAPT_SIZE && e > I7_CAPT)
        return 0;
    return 1;
}

/* the result, for the log page and the preview */
static uint32_t capt_px, capt_us;
static unsigned capt_why;

static void capt_log(void)
{
#ifndef M7_PREVIEW
    volatile uint64_t *log = (volatile uint64_t *)I7_LOG;
    log[I7_LOG_CAPT] = (uint64_t)capt_px << 32 | (uint64_t)I7_LOG_TAG << 16 | (uint64_t)capt_why << 8 |
                       (uint64_t)(unsigned)capt_n;
    log[I7_LOG_CAPT + 1] = capt_us;
#endif
}

static int capt_end(uint64_t t0, unsigned why, uint32_t px)
{
    capt_why = why;
    capt_px = px;
    capt_fail = why != I7_CAPT_OK && why != I7_CAPT_BLACK;
    if (capt_fail) {
        star_own = 1;
        star_x = I7_ONE_STAR_X;
        star_y = I7_ONE_STAR_Y;
        star_r = I7_ONE_STAR_R;
    }
    uint64_t d = m7_now_us() - t0;
    capt_us = d > 0xffffffffU ? 0xffffffffU : (uint32_t)d;
    capt_log();
    return capt_n;
}

/* A pixel (the low 24 bits) counts as picture when its largest channel is at least 8: JPEG
 * leaves 1..7 values in background blocks (the stock logo.jpg has ~22K of them, on PIL's
 * decoder; S-Boot's is untested), which would widen and merge the bands. Only the bands' boxes
 * are found that way; what is copied is every pixel in them, unthresholded. */
#define I7_PIX2 0x00f8f8f800f8f8f8ULL

static void capt_drop(void)
{
    for (unsigned i = 0; i < I7_CAPT_BANDS; i++)
        (void)sb7_put(SB7_T_IMAGE, SB7_S_CAPT0 + i, 0, 0);
    capt_n = 0;
    capt_sam = -1;
    capt_star = -1;
    capt_fail = 0;
    star_own = 0;
}

/* the wordmark: the band across `row`, no taller than a line of text and I7_NAT_WORD_ASPECT times
 * as wide as it is tall (never the star), or -1 */
static int capt_word(unsigned row)
{
    for (int i = 0; i < capt_n; i++) {
        unsigned h = CB[i].y1 - CB[i].y0, w = CB[i].x1 - CB[i].x0;
        if (i != capt_star && CB[i].y0 <= row && CB[i].y1 > row && h <= I7_NAT_SAM_MAX_H &&
            w >= I7_NAT_WORD_ASPECT * h)
            return i;
    }
    return -1;
}

int intro7_capture(void)
{
    uint64_t t0 = m7_now_us();
    capt_drop();
    if (!capt_free())
        return capt_end(t0, I7_CAPT_NOT_FREE, 0);
    /* pass 1 (one read of the uncached panel, two pixels a load): each row's picture extent */
    const volatile uint64_t *fb = I7_FB;
    for (unsigned y = 0; y < G7_H; y++) {
        unsigned x0 = G7_W, x1 = 0;
        const volatile uint64_t *r = fb + (uint64_t)y * (G7_W / 2);
        for (unsigned i = 0; i < G7_W / 2; i++) {
            uint64_t v = r[i];
            if (!(v & I7_PIX2))
                continue;
            unsigned a = (v & (I7_PIX2 & 0xffffffffU)) ? 2 * i : 2 * i + 1;
            if (a < x0)
                x0 = a;
            x1 = (v & (I7_PIX2 & ~0xffffffffULL)) ? 2 * i + 2 : 2 * i + 1;
        }
        row_x0[y] = (uint16_t)x0;
        row_x1[y] = (uint16_t)x1;
    }
    /* the bands: rows joined across black gaps of up to `gap` rows; more than I7_CAPT_BANDS of
     * them and the gap doubles */
    int n = 0;
    for (unsigned gap = I7_CAPT_GAP;; gap *= 2) {
        n = 0;
        unsigned last = 0;
        for (unsigned y = 0; y < G7_H && n <= I7_CAPT_BANDS; y++) {
            unsigned x0 = row_x0[y], x1 = row_x1[y];
            if (x1 <= x0)
                continue;
            if (!n || y - last > gap) {
                if (n == I7_CAPT_BANDS) {
                    n++;
                    break;
                }
                CB[n].y0 = (uint16_t)y;
                CB[n].x0 = (uint16_t)x0;
                CB[n].x1 = (uint16_t)x1;
                n++;
            }
            struct capt_band *c = &CB[n - 1];
            c->y1 = (uint16_t)(y + 1);
            if (x0 < c->x0)
                c->x0 = (uint16_t)x0;
            if (x1 > c->x1)
                c->x1 = (uint16_t)x1;
            last = y;
        }
        if (n <= I7_CAPT_BANDS)
            break;
    }
    /* each box grows by I7_CAPT_PAD (one JPEG block: the faint values sit in the text's blocks),
     * vertically never past the middle of the black gap to the next band, so boxes never overlap */
    unsigned prev_y1 = 0;
    for (int i = 0; i < n; i++) {
        unsigned y0 = CB[i].y0, y1 = CB[i].y1;
        unsigned lo = i ? (prev_y1 + y0 + 1) / 2 : 0, hi = i + 1 < n ? (y1 + CB[i + 1].y0) / 2 : G7_H;
        prev_y1 = y1;
        CB[i].y0 = (uint16_t)(y0 >= lo + I7_CAPT_PAD ? y0 - I7_CAPT_PAD : lo);
        CB[i].y1 = (uint16_t)(y1 + I7_CAPT_PAD <= hi ? y1 + I7_CAPT_PAD : hi);
        CB[i].x0 = (uint16_t)(CB[i].x0 >= I7_CAPT_PAD ? CB[i].x0 - I7_CAPT_PAD : 0);
        CB[i].x1 = (uint16_t)(CB[i].x1 + I7_CAPT_PAD <= G7_W ? CB[i].x1 + I7_CAPT_PAD : G7_W);
    }
    uint64_t px = 0;
    for (int i = 0; i < n; i++)
        px +=(uint64_t)(CB[i].y1 - CB[i].y0) * (CB[i].x1 - CB[i].x0);
    uint32_t px32 = px > 0xffffffffU ? 0xffffffffU : (uint32_t)px;
    if (!n)
        return capt_end(t0, I7_CAPT_BLACK, 0);
    if (px > I7_CAPT_MAX_PX)
        return capt_end(t0, I7_CAPT_TOO_BIG, px32);
    /* pass 2: each band as an sb7 image, premultiplied with alpha = its largest channel, so over
     * black (the intro's base at t = 0) it is exactly what S-Boot drew */
    uintptr_t at = I7_CAPT;
    for (int i = 0; i < n; i++) {
        unsigned w = CB[i].x1 - CB[i].x0, h = CB[i].y1 - CB[i].y0;
        /* a star-shaped box also sums its alpha (a, a x, a y) and its red, green and blue */
        int st = capt_star < 0 && w >= I7_STAR_MIN && w <= I7_STAR_MAX && h >= I7_STAR_MIN && h <= I7_STAR_MAX &&
                 4U * w >= 3U * h && 4U * h >= 3U * w;
        uint64_t sa = 0, sax = 0, say = 0, sr = 0, sg = 0, sb = 0;
        struct sb7_image *im = (struct sb7_image *)at;
        im->w = (uint16_t)w;
        im->h = (uint16_t)h;
        im->format = SB7_ARGB;
        im->enc = SB7_RAW;
        im->flags = 0;
        im->nx = 0;
        im->ny = 0;
        im->nw = (uint16_t)w;
        im->nh = (uint16_t)h;
        im->data_size = w * h * 4U;
        uint32_t *d = (uint32_t *)(im + 1);
        const volatile uint32_t *fb32 = (const volatile uint32_t *)I7_FB;
        for (unsigned y = 0; y < h; y++) {
            const volatile uint32_t *r = fb32 + (uint64_t)(CB[i].y0 + y) * G7_W + CB[i].x0;
            for (unsigned x = 0; x < w; x++) {
                uint32_t c = r[x] & 0xffffffU, m = c >> 16;
                if (((c >> 8) & 255U) > m) m = (c >> 8) & 255U;
                if ((c & 255U) > m) m = c & 255U;
                *d++ = m << 24 | c;
                if (st) {
                    sa += m;
                    sax += (uint64_t)m * x;
                    say += (uint64_t)m * y;
                    sr += c >> 16;
                    sg += (c >> 8) & 255U;
                    sb += c & 255U;
                }
            }
        }
        /* blue (not white, grey, green or teal), not a solid tile, in the logo block */
        if (st && sa && 2U * sb >= 3U * sr && sb >= sg &&
            (uint64_t)I7_STAR_FILL_DIV * sa <= 255U * (uint64_t)w * h &&
            (uint64_t)CB[i].y0 * sa + say < (uint64_t)I7_NAT_SPLIT * sa) {
            float r = __builtin_sqrtf((float)sa * (1.0f / 255.0f) / I7_STAR_AREA);
            capt_star = i;
            star_own = 1;
            star_x = (float)CB[i].x0 + 0.5f + (float)sax / (float)sa;
            star_y = (float)CB[i].y0 + 0.5f + (float)say / (float)sa;
            star_r = r < I7_STAR_R_MIN ? I7_STAR_R_MIN : r > I7_STAR_R_MAX ? I7_STAR_R_MAX : r;
        }
        uint32_t size = (uint32_t)sizeof(*im) + im->data_size;
        if (!sb7_put(SB7_T_IMAGE, SB7_S_CAPT0 + (unsigned)i, im, size)) {
            capt_drop();
            return capt_end(t0, I7_CAPT_TABLE_FULL, px32);
        }
        at += ((uintptr_t)size + 63U) & ~(uintptr_t)63U;
    }
    capt_n = n;
    /* the wordmark: Samsung's across panel row I7_NAT_SAM_ROW (it glides and grows to I7_NAT_SAM_W),
     * else Starbite's across I7_NAT_WORD_ROW (it keeps its size) */
    capt_sam = capt_word(I7_NAT_SAM_ROW);
    sam_w = I7_NAT_SAM_W;
    if (capt_sam < 0) {
        capt_sam = capt_word(I7_NAT_WORD_ROW);
        if (capt_sam >= 0)
            sam_w = (float)(CB[capt_sam].x1 - CB[capt_sam].x0);
    }
    return capt_end(t0, I7_CAPT_OK, px32);
}

void intro7_release(void)
{
    capt_drop();
}

/* The captured bands at t. The rest of the logo block fades and rises as #gs9, the bands below it
 * ("Powered by ...") fade as #pow. The wordmark (capt_sam) is the only one on the screen: it stays
 * while "Galaxy S9" goes, then moves (and Samsung's grows) into the concept's wordmark box (sam_w
 * wide, centred on the capsule at I7_CAP_X, I7_CAP_Y) by I7_MELT0, and shrinks with the capsule's
 * width (W0 -> W1) as it fades out over the melt, where the island takes over. The splash's star
 * (capt_star) fades in place over I7_STAR_FADE as the wallpaper's star lights on top of it. */
static void capt_items(struct g7_layer *l, float t)
{
    float ua = 1.0f - c01(t / I7_GS9_FADE), dy = -36.0f * m7_eout(c01(t / I7_GS9_RISE));
    float la = 1.0f - c01(t / I7_POW_FADE);
    for (int i = 0; i < capt_n; i++) {
        float x = (float)CB[i].x0, y = (float)CB[i].y0;
        float w = (float)(CB[i].x1 - CB[i].x0), h = (float)(CB[i].y1 - CB[i].y0);
        if (i == capt_sam) {
            float p = m7_ease(c01((t - I7_NAT_MOVE0) / (float)(I7_MELT0 - I7_NAT_MOVE0)));
            float m = m7_ease(c01((t - I7_MELT0) / I7_MELT_MS));
            float k = lerpf(1.0f, sam_w / w, p) * lerpf(1.0f, I7_CAP_W1 / I7_CAP_W0, m);
            float cx = lerpf(x + 0.5f * w, I7_CAP_X, p), cy = lerpf(y + 0.5f * h, I7_CAP_Y, p);
            add_image(l, I7_KEY(16 + i), SB7_S_CAPT0 + (unsigned)i, cx - 0.5f * w * k, cy - 0.5f * h * k, w * k, h * k,
                      0xffffffffU, 1.0f - m);
            continue;
        }
        if (i == capt_star) {
            add_image(l, I7_KEY(16 + i), SB7_S_CAPT0 + (unsigned)i, x, y, w, h, 0xffffffffU,
                      1.0f - c01(t / I7_STAR_FADE));
            continue;
        }
        int up = CB[i].y0 < I7_NAT_SPLIT;
        add_image(l, I7_KEY(16 + i), SB7_S_CAPT0 + (unsigned)i, x, y + (up ? dy : 0.0f), w, h, 0xffffffffU,
                  up ? ua : la);
    }
}

/* ---- the wallpaper ------------------------------------------------------------------------------ */

/* WALL_FS's reveal is m = 1 - smoothstep(rr - rf, rr, r): once rr - rf is beyond every pixel's
 * distance from the final star (1290, 232), the furthest being the corner (0, 2960) at 3017.6 px,
 * m is 1 everywhere, as with the final rr (1e5). */
#define I7_REV_COVER 3018.0f

static void wall_at(struct g7_wall *w, float t)
{
    /* star flight along a quadratic path, bloom on arrival */
    float e = m7_ease(c01((t - I7_FLY0) / I7_FLY_MS)), a = 1.0f - e, sx, sy;
    if (star_own) {
        /* from the captured star (or One mark's, after a failed capture): a short arc up and right */
        sx = a * a * star_x + 2.0f * a * e * I7_FLY_STAR_CX + e * e * G7_WALL_FINAL_SX;
        sy = a * a * star_y + 2.0f * a * e * I7_FLY_STAR_CY + e * e * G7_WALL_FINAL_SY;
    } else {
        sx = a * a * M7_STAR0_X + 2.0f * a * e * I7_FLY_CX + e * e * G7_WALL_FINAL_SX;
        sy = a * a * M7_STAR0_Y + 2.0f * a * e * I7_FLY_CY + e * e * G7_WALL_FINAL_SY;
    }
    float bloom = sinpi01(c01((t - I7_BLOOM0) / I7_BLOOM_MS));
    float rr = t < I7_REV0 ? 0.0f : I7_REV_R * m7_eout(c01((t - I7_REV0) / I7_REV_MS));
    int final = t >= I7_FLY0 + I7_FLY_MS && t >= I7_SR0 + I7_SR_MS && t >= I7_SW0 + I7_SW_MS &&
                t >= I7_BLOOM0 + I7_BLOOM_MS && rr - I7_REV_FEATHER >= I7_REV_COVER;
    if (final) {
        /* what the shader draws is the final wallpaper: say so exactly, so glass stops rebuilding
         * it (the concept's own values here differ only in rr, 3900 against 1e5, and in the last
         * float bits of sr) */
        w->sx = G7_WALL_FINAL_SX;
        w->sy = G7_WALL_FINAL_SY;
        w->si = G7_WALL_FINAL_SI;
        w->sr = G7_WALL_FINAL_SR;
        w->sw = G7_WALL_FINAL_SW;
        w->rr = G7_WALL_FINAL_RR;
        w->rf = G7_WALL_FINAL_RF;
        w->amb = G7_WALL_FINAL_AMB;
        return;
    }
    w->sx = sx;
    w->sy = sy;
    w->si = c01(t / I7_SI_RAMP) * (1.0f + 0.45f * bloom);
    w->sr = lerpf(star_own ? star_r : M7_STAR0_R, 54.0f, m7_ease(c01((t - I7_SR0) / I7_SR_MS)));
    w->sw = 0.72f * m7_ease(c01((t - I7_SW0) / I7_SW_MS));
    w->rr = rr;
    w->rf = I7_REV_FEATHER;
    w->amb = 1.0f;
}

/* ---- introAt(t) --------------------------------------------------------------------------------- */

static void xf_at(struct m7_xf *x, float t)
{
    x->menu_a = 1.0f;                                   /* menu.style.opacity = 1 */
    /* the island: pillEl._ga = pillA = t >= 1050; pillY = 1136 (1 - spA((t - 1050) / 1000, .45, .8)) */
    x->pill_a = t >= I7_ISLAND0 ? 1.0f : 0.0f;
    x->pill_dy = I7_ISLAND_DY * (1.0f - spa_ms(t, I7_ISLAND0, 0.45f, 0.8f));
    x->pill_in_a = c01((t - I7_ISLAND_IN0) / I7_ISLAND_IN_MS);
    /* menu content springs in once the island has passed */
    float ta = spa_ms(t, I7_TITLE0, 0.6f, 0.86f);
    x->brand_dy = I7_TITLE_DY * (1.0f - ta);
    x->brand_a = c01(ta * 1.3f);
    x->sub_dy = I7_SUB_DY * (1.0f - spa_ms(t, I7_SUB0, 0.6f, 0.86f));
    x->sub_a = c01((t - I7_SUB0) / I7_FADE_MS);
    x->foot_a = c01((t - I7_FOOT0) / I7_FADE_MS);
    x->status_a = c01((t - I7_STATUS0) / I7_FADE_MS);
    for (int i = 0; i < M7_MAX_SECTIONS; ++i) {
        float s = spa_ms(t, (float)(I7_CARDS0 + i * I7_CARD_STAGGER), 0.55f, 0.8f);
        x->card_dy[i] = I7_CARD_DY * (1.0f - s);
        x->card_s[i] = 0.96f + 0.04f * s;
        x->card_a[i] = c01(s * 1.6f);
    }
    float tb = spa_ms(t, I7_DOCK0, 0.55f, 0.8f);
    x->dock_dy = I7_DOCK_DY * (1.0f - tb);
    x->dock_a = c01(tb * 1.6f);
    x->lens_a = c01((t - I7_LENS0) / I7_ISLAND_IN_MS);  /* LB.a.snap(clamp((t - 1850) / 300)) */
}

void intro7_frame(struct g7_frame *f, float t)
{
    if (t < 0.0f)
        t = 0.0f;
    struct m7_xf xf;
    xf_at(&xf, t);
    m7_build(f, &xf);

    wall_at(&f->wall, t);
    f->dim = lerpf(1.0f, 0.8f, m7_ease(c01((t - I7_DIM0) / I7_DIM_MS)));
    /* glass wordmark: white drains (220-740), the letters melt into one capsule (700-1050) */
    float m = m7_ease(c01((t - I7_MELT0) / I7_MELT_MS));
    f->word.tf = 1.0f - m7_ease(c01((t - I7_DRAIN0) / I7_DRAIN_MS));
    f->word.tm = m;
    /* from the captured splash there is no glass wordmark: Samsung's own (capt_items) takes its
     * place, and the island comes out of where it fades; after a failed capture neither (the
     * glass wordmark is SAMSUNG's: the capsule is born empty) */
    f->word.ta = t >= I7_ISLAND0 || capt_n || capt_fail ? 0.0f : 1.0f;
    f->word.cap[0] = I7_CAP_X;
    f->word.cap[1] = I7_CAP_Y;
    f->word.cap[2] = lerpf(I7_CAP_W0, I7_CAP_W1, m);
    f->word.cap[3] = lerpf(I7_CAP_H0, I7_CAP_H1, m);

    /* m7_build points f->over at menu7's own (writable) over layer */
    if (capt_n)
        capt_items((struct g7_layer *)f->over, t);
    else if (!capt_fail)
        splash_items((struct g7_layer *)f->over, t);
}

/* ---- the run ------------------------------------------------------------------------------------ */

#define I7_FRAME_US (1000000U / M7_FPS)
#define I7_POLL_US 2000U
/* Keys already down when the intro starts are ignored until every key has been up this long
 * (as anim.c's keys_armed; longer than m7_input's debounce, so the release events of those keys
 * are dropped too). */
#define I7_ARM_US (3U * M7_DEBOUNCE_MS * 1000U)

static struct g7_frame frame;

/* What an input event does to the intro (the concept's key() and pointerdown in 'intro' mode):
 * 0 nothing, I7_SKIPPED, I7_LAB. Up / Down skip on press, Power and Bixby on release or hold
 * (keyDown only starts their hold timers), a touch on contact. */
static int intro_event(const struct m7_ev *ev, int armed)
{
    switch (ev->type) {
    case M7_EV_TOUCH_DOWN:
        return I7_SKIPPED;
    case M7_EV_UP:
    case M7_EV_DOWN:
    case M7_EV_POWER:
    case M7_EV_POWER_HOLD:
    case M7_EV_BIXBY:
        return armed ? I7_SKIPPED : 0;
    case M7_EV_BIXBY_HOLD:
        return armed ? I7_LAB : 0;
    default:                        /* M7_EV_POWER_DOWN (the press visual is the menu's), moves, ups */
        return 0;
    }
}

int intro7_run(void)
{
    (void)intro7_capture();         /* before the first frame overwrites S-Boot's splash */
    m7_prepare_boot();
    uint64_t t0 = m7_now_us(), last = t0, up_since = t0;
    int armed = 0, res = I7_DONE, first = 1;
    for (;;) {
        uint64_t now = m7_now_us();
        float t = (float)(now - t0) * 0.001f;
        if (t >= (float)I7_MS)
            break;
        float dt = (float)(now - last) * 1e-6f;
        last = now;
        /* frame(): introAt(t), every spring steps (by the real dt: a slow frame does not slow the
         * island or the list down), update + render */
        m7_advance(dt);
        intro7_frame(&frame, t);
        frame.flags = first ? G7_F_FULL : 0;        /* the first frame replaces S-Boot's splash */
        first = 0;
        m7_frame_timed(&frame, t < 1600.f ? M7_PH_INTRO_WALL : M7_PH_INTRO_GLASS);
        /* until the next frame: input every 2 ms (frames are dropped when late, never slowed) */
        uint64_t next = now + I7_FRAME_US;
        for (;;) {
            struct m7_ev ev;
            while (m7_input(&ev)) {
                int r = intro_event(&ev, armed);
                if (r) {
                    res = r;
                    goto finish;
                }
            }
            uint64_t n = m7_now_us();
            if (!armed) {
                if (hw_keys())
                    up_since = n;
                else if (n - up_since >= I7_ARM_US)
                    armed = 1;
            }
            if (n >= next)
                break;
            uint64_t w = next - n;
            hw_wait_us(w < I7_POLL_US ? w : I7_POLL_US);
        }
    }
finish:
    /* finishIntro(skipped), then the frame it ends in: the menu's first frame */
    m7_finish_intro(res != I7_DONE);
    {
        uint64_t now = m7_now_us();
        m7_advance((float)(now - last) * 1e-6f);
    }
    m7_build(&frame, 0);
    frame.flags = first ? G7_F_FULL : 0;
    m7_frame_timed(&frame, M7_PH_INTRO_GLASS);
    intro7_release();               /* I7_CAPT is free again: nothing may find the bands now */
    return res;
}
