/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass renderer: private declarations shared by glass.c and glass_*.c.
 * Not an interface: only the glass files include it (the interface is glass.h).
 *
 * Files:
 *   glass.c        the API: init, frame diff and dirty rects, compose driver, freeze, present
 *   glass_wall.c   WALL_FS (the static field, the wallpaper for any struct g7_wall), the four
 *                  BLUR_FS passes of renderWall into blur A and B, the frost plane A5, BASE
 *   glass_shade.c  GLASS_FS: shapes (sdSq, shadow, coverage, shade(), lens), the glass wordmark,
 *                  the dither, one row at a time
 *   glass_draw.c   the content items (text, images, tiles, rects, shadows, arcs, strokes), the
 *                  image and tile cache (G7_CACHE)
 *
 * Built with FP/SIMD (CFLAGS_V7). No libm: the transcendental functions below are polynomials
 * accurate to ~1e-7 relative (enough for 8-bit output); sqrt is fsqrt.
 */
#pragma once
#include <stdint.h>
#include <arm_neon.h>
#include "glass.h"
#include "sb7.h"

uint64_t hw_ticks(void);
uint64_t hw_freq(void);

/* ---- planes ---- */
#define GW G7_W
#define GH G7_H
#define WW 720                      /* WALL_FS's texture (renderWall W x H) */
#define WH 1480
#define AW 360                      /* blur A */
#define AH 740
#define BW 180                      /* blur B */
#define BH 370
/* The canvas (MV4 route 2): every layer below the content is composed on the half-resolution grid,
 * canvas pixel (i, j) = the panel point (2i + 1, 2j + 1), the wallpaper texture's own texels and the
 * grid the glass has been shaded on since round 3. Geometry stays in panel px. The present scales
 * the canvas 2x to the panel (bilinear, the weights the glass's upsample had) and draws the content
 * items there at panel resolution, so text, icons and tiles stay sharp. G7_CANVAS, G7_SHARP (GLB,
 * the frozen base), G7_BASE (BG8), TPL and SOFT are canvas-sized. */
#define CW WW
#define CH WH
/* a panel range [p0, p1) -> the canvas samples it can change, with one sample of margin */
static inline int c_lo(int p, int n) { int v = (p >> 1) - 1; return v < 0 ? 0 : v > n ? n : v; }
static inline int c_hi(int p, int n) { int v = ((p + 1) >> 1) + 1; return v < 0 ? 0 : v > n ? n : v; }

/* G7_POOL layout (18 MiB). Sizes in bytes. */
#define PL_WALL_BYTES (4UL * WW * WH)                   /* 4 262 400 */
#define PL_A_BYTES (4UL * AW * AH)                      /* 1 065 600 */
#define PL_B_BYTES (4UL * BW * BH)                      /*   266 400 */
#define P_FIELD  (G7_POOL)                              /* the static field, packed 10/11/11 */
#define P_SOFT   (P_FIELD + PL_WALL_BYTES)              /* SOFT 720 x 1480 (bake: blur temps) */
#define P_TMP    (P_SOFT + PL_WALL_BYTES)               /* the wallpaper texture, freeze temp */
#define P_A      (P_TMP + PL_WALL_BYTES)                /* blur A, 8-bit */
#define P_A5     (P_A + PL_A_BYTES)                     /* frost taps on A, 10:10:10 */
#define P_B      (P_A5 + PL_A_BYTES)                    /* blur B, 8-bit */
#define P_TAB    (P_B + PL_B_BYTES)                     /* tables, 1 MiB: gw_wall's radial and column
                                                         * tables 0 .. 0x2B200, glass.c's row buffers
                                                         * 0x2C000, sdSq g(t) 0x40000, specular columns
                                                         * 0x50000, half rows 0xDD000 */
#define P_TAB_BYTES 0x100000UL
#define P_SCR    (P_TAB + P_TAB_BYTES)                  /* scratch for decoding, 1 MiB */
#define P_SCR_BYTES 0x100000UL
#define P_END    (P_SCR + P_SCR_BYTES)
_Static_assert(P_END - G7_POOL <= G7_POOL_SIZE, "glass pool overflow");

/* ---- math ---- */
static inline float g_min(float a, float b) { return a < b ? a : b; }
static inline float g_max(float a, float b) { return a > b ? a : b; }
static inline float g_abs(float a) { return __builtin_fabsf(a); }
static inline float g_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float g_sat(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
static inline float g_mix(float a, float b, float t) { return a + (b - a) * t; }
static inline float g_floor(float x) { return __builtin_floorf(x); }
static inline float g_fract(float x) { return x - __builtin_floorf(x); }
static inline float g_sqrt(float x) { return __builtin_sqrtf(x); }
static inline int g_ifloor(float x) { return (int)__builtin_floorf(x); }
/* GLSL smoothstep, including e0 > e1 (the concept relies on it: core = smoothstep(1, .86, ast)) */
static inline float g_smooth(float e0, float e1, float x)
{
    float t = g_sat((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

/* 2^x: round to nearest integer, degree-7 polynomial on [-.5, .5] (rel. error ~5e-9). */
static inline float g_exp2(float x)
{
    if (x < -125.f)
        return 0.f;
    if (x > 127.f)
        x = 127.f;
    float fi = __builtin_floorf(x + .5f);
    float f = x - fi;
    float p = 1.52527338e-5f;
    p = p * f + 1.54035304e-4f;
    p = p * f + 1.33335581e-3f;
    p = p * f + 9.61812911e-3f;
    p = p * f + 5.55041087e-2f;
    p = p * f + 2.40226507e-1f;
    p = p * f + 6.93147181e-1f;
    p = p * f + 1.f;
    union { float f; int32_t i; } u;
    u.f = p;
    u.i += (int32_t)fi << 23;
    return u.f;
}
static inline float g_exp(float x) { return g_exp2(x * 1.44269504f); }
/* log2 for x > 0: mantissa in [sqrt(.5), sqrt(2)), atanh series (error ~1e-9). */
static inline float g_log2(float x)
{
    union { float f; int32_t i; } u;
    u.f = x;
    int32_t e = ((u.i >> 23) & 255) - 127;
    u.i = (u.i & 0x007fffff) | 0x3f800000;  /* m in [1, 2) */
    float m = u.f;
    if (m > 1.41421356f) {
        m *= .5f;
        e++;
    }
    float s = (m - 1.f) / (m + 1.f), s2 = s * s;
    float p = s2 * (1.f / 9.f) + (1.f / 7.f);
    p = p * s2 + (1.f / 5.f);
    p = p * s2 + (1.f / 3.f);
    p = p * s2 + 1.f;
    return (float)e + 2.88539008f * s * p;       /* 2 / ln 2 */
}
static inline float g_pow(float x, float y) { return x <= 0.f ? 0.f : g_exp2(y * g_log2(x)); }
/* sin(x), any x: reduce to [-pi, pi], fold to [-pi/2, pi/2], odd series to x^13. */
static inline float g_sin(float x)
{
    const float pi = 3.14159265f, tpi = 6.28318531f;
    x -= tpi * __builtin_floorf(x / tpi + .5f);
    if (x > pi * .5f)
        x = pi - x;
    else if (x < -pi * .5f)
        x = -pi - x;
    float x2 = x * x;
    float p = -1.60590438e-10f;
    p = p * x2 + 2.50521084e-8f;
    p = p * x2 - 2.75573192e-6f;
    p = p * x2 + 1.98412698e-4f;
    p = p * x2 - 8.33333333e-3f;
    p = p * x2 + 1.66666667e-1f;
    return x - x * x2 * p;
}
static inline float g_cos(float x) { return g_sin(x + 1.57079633f); }
/* atan2 in radians, |error| < 1e-5 */
static inline float g_atan2(float y, float x)
{
    float ax = g_abs(x), ay = g_abs(y);
    float mx = g_max(ax, ay), mn = g_min(ax, ay);
    if (mx == 0.f)
        return 0.f;
    float a = mn / mx, s = a * a;
    float r = ((((-0.0117212f * s + 0.05265332f) * s - 0.11643287f) * s + 0.19354346f) * s - 0.33262347f) * s + 0.99997726f;
    r *= a;
    if (ay > ax)
        r = 1.57079633f - r;
    if (x < 0.f)
        r = 3.14159265f - r;
    return y < 0.f ? -r : r;
}
/* erf, the Abramowitz-Stegun style rational used by box-shadow renderers (|error| < 5e-4) */
static inline float g_erf(float x)
{
    float s = x < 0.f ? -1.f : 1.f, a = g_abs(x);
    float t = 1.f + (0.278393f + (0.230389f + 0.000972f * a + 0.078108f * a * a) * a) * a;
    t *= t;
    return s - s / (t * t);
}

/* 0xAARRGGBB helpers */
#define CA(c) (((c) >> 24) & 255U)
#define CR(c) (((c) >> 16) & 255U)
#define CG(c) (((c) >> 8) & 255U)
#define CB(c) ((c) & 255U)
/* x / 255 rounded, for x in 0 .. 255 * 255 */
static inline uint32_t g_div255(uint32_t x) { x += 128U; return (x + (x >> 8)) >> 8; }

/* packed 10:10:10 planes (BASE, A5): value in 0..1 per channel */
static inline uint32_t g_pack10(float r, float g, float b)
{
    uint32_t R = (uint32_t)(g_sat(r) * 1023.f + .5f), G = (uint32_t)(g_sat(g) * 1023.f + .5f),
             B = (uint32_t)(g_sat(b) * 1023.f + .5f);
    return R << 20 | G << 10 | B;
}
#define UNP10_R(v) ((float)(((v) >> 20) & 1023U) * (1.f / 1023.f))
#define UNP10_G(v) ((float)(((v) >> 10) & 1023U) * (1.f / 1023.f))
#define UNP10_B(v) ((float)((v) & 1023U) * (1.f / 1023.f))

/* ---- the dither: GLASS_FS hash(gl_FragCoord.xy), gl_FragCoord = (x + .5, 2960 - y - .5) ---- */
extern float g_dxt[GW];                         /* fract((x + .5) * 443.897), glass_shade.c */
static inline float gs_dither_y(int y) { return g_fract(((float)GH - (float)y - .5f) * 441.423f); }
static inline float g_dither(int x, float Y)
{
    float X = g_dxt[x];
    float d = X * (Y + 19.19f) + Y * (X + 19.19f);
    X += d;
    Y += d;
    return g_fract((X + Y) * X);
}
static inline uint32_t g_q255(float v) { return v <= 0.f ? 0U : v >= 255.f ? 255U : (uint32_t)v; }
/* NEON: the dither for 4 columns (g_dxt[x .. x + 3]) of a row (Y = gs_dither_y), and the store's
 * clamp-and-truncate of 4 pixels (values in 0 .. 255 units) to 0xffRRGGBB */
static inline float32x4_t g_fract4(float32x4_t v) { return vsubq_f32(v, vrndmq_f32(v)); }
static inline float32x4_t g_dither4(int x, float Y)
{
    float32x4_t X = vld1q_f32(g_dxt + x), Yv = vdupq_n_f32(Y), k = vdupq_n_f32(19.19f);
    float32x4_t d = vaddq_f32(vmulq_f32(X, vaddq_f32(Yv, k)), vmulq_f32(Yv, vaddq_f32(X, k)));
    X = vaddq_f32(X, d);
    Yv = vaddq_f32(Yv, d);
    return g_fract4(vmulq_f32(vaddq_f32(X, Yv), X));
}
/* n pixels (no overlap), 16 a step */
static inline void g_copy32(uint32_t *d, const uint32_t *s, int n)
{
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        uint32x4x4_t v = vld1q_u32_x4(s + i);
        vst1q_u32_x4(d + i, v);
    }
    for (; i < n; i++)
        d[i] = s[i];
}
/* 8 pixels 0xAARRGGBB -> float r, g, b (0 .. 255), lanes 0..3 and 4..7 */
static inline void g_unpack8(const uint32_t *p, float32x4_t o[3][2])
{
    uint8x8x4_t v = vld4_u8((const uint8_t *)p);   /* bytes B, G, R, A */
    for (int c = 0; c < 3; c++) {
        uint16x8_t w = vmovl_u8(v.val[2 - c]);
        o[c][0] = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w)));
        o[c][1] = vcvtq_f32_u32(vmovl_u16(vget_high_u16(w)));
    }
}
/* 4 pixels -> float r, g, b (0 .. 255) */
static inline void g_unpack4(const uint32_t *p, float32x4_t o[3])
{
    uint32x4_t v = vld1q_u32(p), m = vdupq_n_u32(255U);
    o[0] = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(v, 16), m));
    o[1] = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(v, 8), m));
    o[2] = vcvtq_f32_u32(vandq_u32(v, m));
}
static inline uint32x4_t g_pack4(float32x4_t r, float32x4_t g, float32x4_t b)
{
    uint32x4_t m = vdupq_n_u32(255U);
    uint32x4_t R = vminq_u32(vcvtq_u32_f32(r), m), G = vminq_u32(vcvtq_u32_f32(g), m), B = vminq_u32(vcvtq_u32_f32(b), m);
    return vorrq_u32(vorrq_u32(vdupq_n_u32(0xff000000U), vshlq_n_u32(R, 16)), vorrq_u32(vshlq_n_u32(G, 8), B));
}
void gs_dither_init(void);

/* ---- glass_wall.c ---- */
void gw_field(void);                            /* the static field into P_FIELD (bake) */
int gw_field_valid(void);
void gw_field_lost(void);                       /* P_FIELD was reused (freeze temp) */
void gw_wall(const struct g7_wall *w);          /* WALL_FS for w into P_TMP (8-bit texture) */
void gw_blur(void);                             /* renderWall's blur passes: P_TMP -> A, B; A5 */
void gw_planes(void);                           /* A5 and B on the canvas grid (G7_SHARP + 4 MiB, + 8 MiB): reset, rows built at first use */
void gw_base(void);                             /* G7_BASE = BG8: the texture at panel res x .8, dithered */
int gw_wall_same(const struct g7_wall *a, const struct g7_wall *b);
/* bilinear samples at a panel point (clamp to edge, WebGL texel centres) */
void gw_sample_a(float px, float py, float out[3]);
void gw_sample_a5(float px, float py, float out[3]);
void gw_sample_b(float px, float py, float out[3]);
/* one panel row of A5 and B at pixel centres x0 .. x1 - 1 into rgb triplets */
void gw_row_a5(int y, int x0, int x1, float *rgb);
void gw_row_b(int y, int x0, int x1, float *rgb);
void gw_row_a5_y(float py, int x0, int x1, float *rgb);   /* the same at any panel y */
void gw_row_b_y(float py, int x0, int x1, float *rgb);
void gw_row_a_y(float py, int x0, int x1, float *rgb);
/* a row's texels t0 .. t1 blended vertically at panel py, planar (o, o + stride, o + 2 stride) */
void gw_vrow_a5(float py, int t0, int t1, float *o, int stride);
void gw_vrow_a(float py, int t0, int t1, float *o, int stride);
void gw_vrow_b(float py, int t0, int t1, float *o, int stride);
void gw_row_wall(int y, int x0, int x1, float *rgb);  /* the texture at panel res (BASE not valid) */
void gw_row_wall8(int y, int x0, int x1, float dim, uint32_t *dst);  /* x dim, dithered */
void gw_crow_wall8(int j, int i0, int i1, float dim, uint32_t *dst); /* canvas row j: texels x dim, dithered */
/* rows at an arbitrary panel y (for the half resolution render) */
void gw_row_b_at(float py, int n, float px0, float dpx, float *rgb);
void gw_row_a5_at(float py, int n, float px0, float dpx, float *rgb);
void gw_row_a_at(float py, int n, float px0, float dpx, float *rgb);

/* ---- glass_shade.c ---- */
/* prepared shape sets: the base and over layers at panel resolution, the soft base (freeze, K = 2,
 * half resolution) and the base at half resolution with the panel's uniforms (motion frames) */
#define GS_BASE 0
#define GS_OVER 1
#define GS_SOFT 2
#define GS_HALF 3
#define GS_HALFO 4                                  /* the over layer at half resolution */
#define GS_QB 5                                     /* the base at quarter resolution (motion frames) */
#define GS_QO 6                                     /* the over layer at quarter resolution (motion frames) */
#define GS_NL 7
/* specular column tables: the panel-resolution sets (0, 1) are never shaded (only their geometry is
 * used: the shift's classes), so the quarter sets take their slots */
#define GS_EXSLOTS 5
_Static_assert(0x50000UL + GS_EXSLOTS * 20UL * 1440UL * 4UL <= 0x100000UL, "glass table overflow");
struct gs_ctx {                                 /* the uniforms of one GLASS_FS draw */
    float K, soft, dim, back;
    int opaque;
    float lx, ly, Ldx, Ldy;
};
/* prepare a layer's shapes (sdSq parameters, cull boxes, specular tables). layer 0 base, 1 over,
 * 2 the soft base at half resolution; zoom: #menu's scale about (720, 1332) (1 = none) */
void gs_prepare(int layer, const struct g7_shape *sh, unsigned n, const struct gs_ctx *c, float zoom);
/* shade one row: pixel centres px = px0 + i * grid (grid 1, or 2 for layer 2), py; acc rgb and al
 * in/out; rowa5 / rowb: A5 and B at those pixel centres */
void gs_row(int layer, const struct gs_ctx *c, float py, float px0, int n, float *acc, float *al,
            const float *rowa5, const float *rowb, int s0, int s1);   /* prepared shapes s0 .. s1 - 1 */
void gs_new_frame(void);
int gs_row_span(int layer, float py, int s0, int s1, int *x0, int *x1);
int gs_npre(int layer);                         /* prepared shapes before the layer's first lens */
struct gs_cls {
    int x0, x1;                     /* the span */
    float J[9];                     /* shade()'s linear part */
    float mixf;                     /* frost toward B */
    float oy;                       /* on a top or bottom rim band row: the refraction's y offset */
    int bx0, bx1;                   /* ... over these pixels (the band's straight part) */
    int si;                         /* the prepared shape */
    int has_u;                      /* one span lies under the whole box (a tile on its card): */
    float uJ[9], umixf;             /* its J and mix, for the pixels of the box the shape leaves */
};
/* the row's x spans of prepared shapes s < s1 (box holds py, later wins), with shade()'s linear
 * part J (out = J frost) and the frost's mix toward B: the moving frames' frost correction */
int gs_classes(int layer, float py, int s1, struct gs_cls *o, int max);
uint32_t gs_class_mask(int layer, float py, int s1);
/* shape si's coverage as shade() takes it (edge AA x alpha x the viewport fade) on the row at py,
 * pixels x0 .. x1 - 1; returns 1 on a corner or top / bottom edge row (it changes from row to row) */
int gs_cover_row(int layer, int si, float py, int x0, int x1, float *cov);
/* the same at n points px0 + k dpx (the canvas grid) */
int gs_cover_row_g(int layer, int si, float py, float px0, float dpx, int n, float *cov);
int gs_nshapes(int layer);
/* the glass wordmark over a base row */
void gs_word_prepare(const struct g7_word *w, const struct gs_ctx *c);
int gs_word_rows(int *y0, int *y1, int *x0, int *x1);
void gs_word_row(const struct gs_ctx *c, float py, int x0, int n, float *acc);
void gs_word_row_g(const struct gs_ctx *c, float py, float px0, float dpx, int n, float *acc);
/* dither and store: out = floor(255 acc + hash) for gl_FragCoord (x + .5, 2960 - y - .5) */
void gs_store_row(uint32_t *dst, int y, int x0, int n, const float *acc);
/* over layer composite: dst = acc al + dst (1 - al), dithered */
void gs_over_row(uint32_t *dst, int y, int x0, int n, const float *acc, const float *al);
int gs_load_height(void);

/* ---- glass_draw.c ---- */
struct gd_target {
    uint32_t *px;                               /* pixel (0, 0) of the target */
    int stride;                                 /* in pixels */
    int x0, y0, x1, y1;                         /* the region drawn now (target px, x1 / y1 exclusive) */
    float s, ox, oy;                            /* target = panel * s + o */
    int premul;                                 /* 1: a transparent premultiplied buffer */
};
void gd_items(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t);
void gd_items_range(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t, unsigned i0,
                    unsigned i1);                /* items i0 .. i1 - 1 */
void gd_items_list(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t,
                   const uint16_t *idx, unsigned n);   /* items idx[0 .. n - 1], in that order */
/* an item's drawn bounds in panel px (x0, y0, x1, y1), 0 if it draws nothing */
int gd_bounds(const struct g7_layer *l, const struct g7_item *it, int16_t b[4]);
int gd_init(void);
const struct sb7_font *gd_font(unsigned id);

/* ---- glass.c ---- */
void g7_poll(void);                             /* the g7_set_poll hook, between rows (glass.c) */
extern float g_row_tmp[];                       /* shared row scratch (see glass.c) */
