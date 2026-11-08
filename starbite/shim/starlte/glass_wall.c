/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass: the wallpaper (the concept's WALL_FS at 720 x 1480), the four BLUR_FS
 * passes of renderWall into blur A (360 x 740) and blur B (180 x 370), the frost plane A5 and
 * BASE (the wallpaper texture bilinear at panel resolution). docs/MENU7-PORT.md 4.1.
 *
 * WALL_FS is split into what does not depend on the uniforms (the night field, washes, silk,
 * vignette: P_FIELD, computed once) and the star / reveal terms, evaluated per frame from row and
 * column tables (the core's pow(a + 1e-4, .62) and the spikes are separable) and a radial table
 * (lit, halo, reveal mask) interpolated at 1/4 px. The tiny stars are a list (WALL_FS adds one
 * only where its cell's hash > .984). Output: an 8-bit RGBA texture, as the concept's FBO.
 *
 * BLUR_FS: every tap is bilinear with clamp to edge; along the pass axis the tap positions have
 * the same fraction for every pixel, so each pass is a discrete kernel of <= 14 integer taps
 * (exactly the bilinear weights), across it the half-texel of a downsampling pass is a 50/50 row
 * blend. Each pass is stored at 8 bits, as the concept's RGBA8 FBOs.
 */
#include "glass_int.h"

#define TEX(a) ((uint32_t *)(a))
static int field_ok;

/* ---- the static field ---- */

/* P_FIELD packing: R 10 bits over [0, .125], G 11 bits over [0, .25], B 11 bits over [0, .5]
 * (the field's maxima are .065, .158, .450) */
static inline uint32_t fpack(float r, float g, float b)
{
    uint32_t R = (uint32_t)(g_sat(r * 8.f) * 1023.f + .5f), G = (uint32_t)(g_sat(g * 4.f) * 2047.f + .5f),
             B = (uint32_t)(g_sat(b * 2.f) * 2047.f + .5f);
    return R << 22 | G << 11 | B;
}
#define FUNP_R(v) ((float)((v) >> 22) * (.125f / 1023.f))
#define FUNP_G(v) ((float)(((v) >> 11) & 2047U) * (.25f / 2047.f))
#define FUNP_B(v) ((float)((v) & 2047U) * (.5f / 2047.f))

/* tiny stars (WALL_FS: cell = floor(p / 46), h = hash(cell) > .984) */
#define NSTAR 96
static struct { float x, y, k; int cx, cy; } star[NSTAR];
static int nstar;

static float whash(float x, float y)   /* WALL_FS hash */
{
    x = g_fract(x * 123.34f);
    y = g_fract(y * 456.21f);
    float d = x * (x + 45.32f) + y * (y + 45.32f);
    x += d;
    y += d;
    return g_fract(x * y);
}

/* The field at one texel (fractional texel coordinates allowed): WALL_FS without the uniforms. */
static void field_at(float ti, float tj, float out[3])
{
    const float ar = 2960.f / 1440.f, s0x = 1290.f / 1440.f, s0y = 232.f / 1440.f;
    float ux = (ti + .5f) * 2.f / 1440.f, uy = (tj + .5f) * 2.f / 1440.f;
    float cx1 = .06f + 1.05f * (1.f - ux) + .05f * g_sin(ux * 3.1f + .6f);
    float cx2 = .72f + .85f * (1.f - ux) + .08f * g_sin(ux * 2.4f + 2.f);
    float gx1 = (ux - .08f) * (ux - .08f), gx2 = (ux - .9f) * (ux - .9f);
    float t = g_smooth(0.f, ar * .95f, uy);
    float br = g_mix(.011f, .002f, t), bg = g_mix(.024f, .005f, t), bb = g_mix(.066f, .017f, t);
    float gy1 = (uy - 1.55f) * (uy - 1.55f), gy2 = (uy - 1.95f) * (uy - 1.95f);
    float dsy = (uy - s0y) * .82f, vy = (uy - ar * .5f) * .5f;
    float dsx = ux - s0x, dd2 = dsx * dsx + dsy * dsy, dd = g_sqrt(dd2);
    float r = br, g = bg, b = bb, k;
    k = g_exp(-dd2 / .13f) * .85f;
    r += .03f * k; g += .075f * k; b += .22f * k;
    float edd = g_exp(-dd / .9f);
    k = edd * .5f;
    r += .016f * k; g += .04f * k; b += .12f * k;
    k = g_exp(-(gx1 + gy1) / .22f);
    r += .016f * k; g += .04f * k; b += .13f * k;
    k = g_exp(-(gx2 + gy2) / .12f);
    r += .012f * k; g += .026f * k; b += .085f * k;
    float s1 = uy - cx1;
    float f1 = s1 < 0.f ? g_exp(-s1 * s1 / (.30f * .30f)) : g_exp(-s1 * s1 / (.075f * .075f));
    k = f1 * (.55f + .45f * g_exp(-dd * .9f));
    r += .022f * k; g += .055f * k; b += .15f * k;
    float s2 = uy - cx2;
    float f2 = s2 > 0.f ? g_exp(-s2 * s2 / (.34f * .34f)) : g_exp(-s2 * s2 / (.09f * .09f));
    k = f2 * .6f;
    r += .03f * k; g += .035f * k; b += .13f * k;
    float vx = (ux - .5f) * 1.25f;
    float v = 1.f - .32f * g_smooth(.55f, 1.15f, g_sqrt(vx * vx + vy * vy));
    out[0] = r * v;
    out[1] = g * v;
    out[2] = b * v;
}

/* Every term of the field is smooth on the scale of tens of texels (the narrowest, the silk's
 * lower edge, has sigma .075 = 54 texels), so it is evaluated on a grid of every 4th texel and
 * interpolated bilinearly in between (error < 1e-4 of the value, below the packing's step). */
#define FQ 4
#define FGW (WW / FQ + 1)
#define FGH (WH / FQ + 1)
void gw_field(void)
{
    uint32_t *F = TEX(P_FIELD);
    float *G = (float *)P_SOFT;                         /* the grid: FGW x FGH rgb (bake temp) */
    for (int b = 0; b < FGH; b++) {
        if (!(b & 31))
            g7_poll();
        for (int a = 0; a < FGW; a++)
            field_at((float)(a * FQ), (float)(b * FQ), G + 3 * (b * FGW + a));
    }
    for (int j = 0; j < WH; j++) {
        if (!(j & 63))
            g7_poll();
        int b = j / FQ;
        float fy = (float)(j - b * FQ) * (1.f / FQ);
        const float *g0 = G + 3 * b * FGW, *g1 = g0 + 3 * FGW;
        uint32_t *row = F + (unsigned)j * WW;
        const float32x4_t wx = { 0.f, .25f, .5f, .75f }, vfy = vdupq_n_f32(fy), z = vdupq_n_f32(0.f), o1 = vdupq_n_f32(1.f);
        const float32x4_t sc[3] = { vdupq_n_f32(8.f), vdupq_n_f32(4.f), vdupq_n_f32(2.f) };
        const float32x4_t mx[3] = { vdupq_n_f32(1023.f), vdupq_n_f32(2047.f), vdupq_n_f32(2047.f) };
        for (int a = 0; a < WW / FQ; a++) {             /* texels 4a .. 4a + 3 */
            const float *p = g0 + 3 * a, *q = g1 + 3 * a;
            uint32x4_t u[3];
            for (int k = 0; k < 3; k++) {
                float l = p[k] + (q[k] - p[k]) * fy, r = p[k + 3] + (q[k + 3] - p[k + 3]) * fy;
                float32x4_t c = vfmaq_f32(vdupq_n_f32(l), vdupq_n_f32(r - l), wx);
                c = vminq_f32(vmaxq_f32(vmulq_f32(c, sc[k]), z), o1);
                u[k] = vcvtq_u32_f32(vfmaq_f32(vdupq_n_f32(.5f), c, mx[k]));
            }
            vst1q_u32(row + 4 * a, vorrq_u32(vorrq_u32(vshlq_n_u32(u[0], 22), vshlq_n_u32(u[1], 11)), u[2]));
            (void)vfy;
        }
    }
    /* the tiny stars */
    nstar = 0;
    for (int cy = 0; cy * 46 < 2960; cy++)
        for (int cx = 0; cx * 46 < 1440; cx++) {
            float h = whash((float)cx, (float)cy);
            if (h <= .984f || nstar >= NSTAR)
                continue;
            star[nstar].x = ((float)cx + .5f + (whash(cx + 3.1f, cy + 3.1f) - .5f) * .7f) * 46.f;
            star[nstar].y = ((float)cy + .5f + (whash(cx + 7.7f, cy + 7.7f) - .5f) * .7f) * 46.f;
            star[nstar].k = .42f * (h - .984f) / .016f;
            star[nstar].cx = cx;
            star[nstar].cy = cy;
            nstar++;
        }
    field_ok = 1;
}
int gw_field_valid(void) { return field_ok; }
void gw_field_lost(void) { field_ok = 0; }

/* the reveal mask is 1 on the whole panel once rr - rf is past its farthest pixel from the star */
static int revealed(const struct g7_wall *w)
{
    float dx = g_max(w->sx, (float)GW - w->sx), dy = g_max(w->sy, (float)GH - w->sy);
    return w->rr - w->rf > g_sqrt(dx * dx + dy * dy) + 2.f;
}
int gw_wall_same(const struct g7_wall *a, const struct g7_wall *b)
{
    int ra = revealed(a), rb = revealed(b);
    if (ra != rb)
        return 0;
    if (!ra && (g_abs(a->rr - b->rr) > 1e-4f || g_abs(a->rf - b->rf) > 1e-4f))
        return 0;
    return g_abs(a->sx - b->sx) <= 1e-4f && g_abs(a->sy - b->sy) <= 1e-4f && g_abs(a->si - b->si) <= 1e-4f &&
           g_abs(a->sr - b->sr) <= 1e-4f && g_abs(a->sw - b->sw) <= 1e-4f && g_abs(a->amb - b->amb) <= 1e-4f;
}

/* ---- the wallpaper for a struct g7_wall ---- */

#define RT_STEP 4.f                     /* radial table: 1/4 px */
#define RT_N 14000                      /* 0 .. 3500 px */

void gw_wall(const struct g7_wall *w)
{
    float *rt = (float *)P_TAB;                         /* RT_N x {lit, halo, m} */
    float *cx1 = rt + 3 * RT_N, *cx2 = cx1 + WW, *cp = cx2 + WW;    /* columns */
    float R = g_max(w->sr, 1.f), si = w->si, amb = w->amb;
    for (int k = 0; k < RT_N; k++) {
        float r = (float)k / RT_STEP;
        rt[3 * k] = 1.f + g_exp(-r / 700.f) * si * 1.2f;
        rt[3 * k + 1] = g_exp(-r / (R * 1.5f)) * .85f + g_exp(-r / (R * 6.f)) * .36f + g_exp(-r / (R * 22.f)) * .2f;
        rt[3 * k + 2] = (1.f - g_smooth(w->rr - w->rf, w->rr, r)) * amb;
    }
    for (int i = 0; i < WW; i++) {
        float dx = g_abs((i + .5f) * 2.f - w->sx);
        cx1[i] = g_exp(-dx / (R * 1.25f));
        cx2[i] = g_exp(-dx / (R * .035f));
        cp[i] = dx / R < 1.2f ? g_pow(dx / R + 1e-4f, .62f) : 9.f;
    }
    const float scr = .40f, scg = .64f, scb = 1.f;
    float ssw = g_smooth(0.f, 1.f, w->sw * 1.6f + .15f);
    float cor = g_mix(scr, 1.f, w->sw) * 1.05f, cog = g_mix(scg, 1.f, w->sw) * 1.05f, cob = g_mix(scb, 1.f, w->sw) * 1.05f;
    float *row = g_row_tmp;                             /* 3 x WW */
    uint32_t *T = TEX(P_TMP);
    const uint32_t *F = TEX(P_FIELD);
    for (int j = 0; j < WH; j++) {
        float py = (j + .5f) * 2.f, dy = py - w->sy, ady = g_abs(dy);
        g7_poll();
        float ry1 = g_exp(-ady / (R * 1.25f)), ry2 = g_exp(-ady / (R * .035f));
        float rp = ady / R < 1.2f ? g_pow(ady / R + 1e-4f, .62f) : 9.f;
        const uint32_t *fr = F + (unsigned)j * WW;
        float uy = py / 1440.f, tiny = 1.f - .6f * g_smooth(.4f, 1.8f, uy);
        /* 4 texels a NEON step: the radial table's two entries per texel by a 4 x 4 transpose */
        {
            const float32x4_t vdy2 = vdupq_n_f32(dy * dy), vsx = vdupq_n_f32(w->sx), k4 = vdupq_n_f32(RT_STEP);
            const float32x4_t one = vdupq_n_f32(1.f), vry1 = vdupq_n_f32(ry1), vry2 = vdupq_n_f32(ry2), vrp = vdupq_n_f32(rp);
            const float32x4_t vssw = vdupq_n_f32(ssw), vsi = vdupq_n_f32(si), k04 = vdupq_n_f32(.4f);
            const float32x4_t kr = vdupq_n_f32(.125f / 1023.f), kg = vdupq_n_f32(.25f / 2047.f), kb = vdupq_n_f32(.5f / 2047.f);
            const float32x4_t vcor = vdupq_n_f32(cor), vcog = vdupq_n_f32(cog), vcob = vdupq_n_f32(cob);
            const float32x4_t vscr = vdupq_n_f32(scr), vscg = vdupq_n_f32(scg), vscb = vdupq_n_f32(scb);
            const float32x4_t ki = vdupq_n_f32(1.f / (.86f - 1.f)), three = vdupq_n_f32(3.f), two = vdupq_n_f32(2.f);
            const float32x4_t vlast = vdupq_n_f32((float)(RT_N - 2)), vend = vdupq_n_f32((float)(RT_N - 1));
            const float32x4_t i0 = { 1.f, 3.f, 5.f, 7.f };     /* (i + k + .5) x 2 = 2i + 2k + 1 */
            for (int i = 0; i < WW; i += 4) {
                float32x4_t dx = vsubq_f32(vaddq_f32(vdupq_n_f32(2.f * (float)i), i0), vsx);
                float32x4_t q = vmulq_f32(vsqrtq_f32(vfmaq_f32(vdy2, dx, dx)), k4);
                uint32x4_t far = vcgeq_f32(q, vend);
                float32x4_t qf = vrndmq_f32(vminq_f32(q, vlast));
                float32x4_t f = vbslq_f32(far, one, vsubq_f32(q, qf));
                int32x4_t kk = vcvtq_s32_f32(qf);
                const float *e0 = rt + 3 * vgetq_lane_s32(kk, 0), *e1 = rt + 3 * vgetq_lane_s32(kk, 1),
                            *e2 = rt + 3 * vgetq_lane_s32(kk, 2), *e3 = rt + 3 * vgetq_lane_s32(kk, 3);
                float32x4_t a0 = vld1q_f32(e0), a1 = vld1q_f32(e1), a2 = vld1q_f32(e2), a3 = vld1q_f32(e3);
                float32x4x2_t t01 = vtrnq_f32(a0, a1), t23 = vtrnq_f32(a2, a3);
                float32x4_t E0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
                float32x4_t E1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
                float32x4_t E2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
                float32x4_t E3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
                float32x4_t b01 = vcombine_f32(vld1_f32(e0 + 4), vld1_f32(e1 + 4)), b23 = vcombine_f32(vld1_f32(e2 + 4), vld1_f32(e3 + 4));
                float32x4_t E4 = vuzp1q_f32(b01, b23), E5 = vuzp2q_f32(b01, b23);
                float32x4_t lit = vfmaq_f32(E0, vsubq_f32(E3, E0), f), halo = vfmaq_f32(E1, vsubq_f32(E4, E1), f),
                            m = vfmaq_f32(E2, vsubq_f32(E5, E2), f), lm = vmulq_f32(lit, m);
                uint32x4_t v = vld1q_u32(fr + i);
                float32x4_t cr = vmulq_f32(vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(v, 22)), kr), lm);
                float32x4_t cg = vmulq_f32(vmulq_f32(vcvtq_f32_u32(vandq_u32(vshrq_n_u32(v, 11), vdupq_n_u32(2047U))), kg), lm);
                float32x4_t cb = vmulq_f32(vmulq_f32(vcvtq_f32_u32(vandq_u32(v, vdupq_n_u32(2047U))), kb), lm);
                float32x4_t spike = vfmaq_f32(vmulq_f32(vry2, vld1q_f32(cx1 + i)), vld1q_f32(cx2 + i), vry1);
                float32x4_t hs = vmulq_f32(vfmaq_f32(halo, spike, k04), vssw);
                float32x4_t ast = vaddq_f32(vld1q_f32(cp + i), vrp);
                float32x4_t t = vminq_f32(vmaxq_f32(vmulq_f32(vsubq_f32(ast, one), ki), vdupq_n_f32(0.f)), one);
                float32x4_t core = vmulq_f32(vmulq_f32(t, t), vfmsq_f32(three, two, t));
                core = vbslq_f32(vcltq_f32(ast, one), core, vdupq_n_f32(0.f));
                float32x4x3_t o;
                o.val[0] = vfmaq_f32(cr, vfmaq_f32(vmulq_f32(vscr, hs), vcor, core), vsi);
                o.val[1] = vfmaq_f32(cg, vfmaq_f32(vmulq_f32(vscg, hs), vcog, core), vsi);
                o.val[2] = vfmaq_f32(cb, vfmaq_f32(vmulq_f32(vscb, hs), vcob, core), vsi);
                vst3q_f32(row + 3 * i, o);
            }
        }
        /* tiny stars of this row's cells, before the reveal mask (col *= m * amb) */
        int cy = g_ifloor(py / 46.f);
        for (int s = 0; s < nstar; s++) {
            if (star[s].cy != cy || g_abs(star[s].y - py) > 7.f)
                continue;
            int i0 = g_ifloor((star[s].x - 7.f) * .5f), i1 = g_ifloor((star[s].x + 7.f) * .5f);
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < WW; i++) {
                float px = (i + .5f) * 2.f;
                if (g_ifloor(px / 46.f) != star[s].cx)
                    continue;
                float ddx = px - star[s].x, ddy = py - star[s].y, sd2 = ddx * ddx + ddy * ddy;
                float dx = px - w->sx, r = g_sqrt(dx * dx + dy * dy);
                float m = (1.f - g_smooth(w->rr - w->rf, w->rr, r)) * amb;
                float k = g_exp(-sd2 / 3.f) * star[s].k * tiny * m;
                row[3 * i] += .7f * k;
                row[3 * i + 1] += .82f * k;
                row[3 * i + 2] += 1.f * k;
            }
        }
        uint32_t *t = T + (unsigned)j * WW;
        {
            const float32x4_t z = vdupq_n_f32(0.f), o1 = vdupq_n_f32(1.f), k = vdupq_n_f32(255.f), h = vdupq_n_f32(.5f);
            for (int i = 0; i < WW; i += 4) {
                float32x4x3_t c = vld3q_f32(row + 3 * i);
                float32x4_t r = vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[0], z), o1), k);
                float32x4_t g = vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[1], z), o1), k);
                float32x4_t b = vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[2], z), o1), k);
                vst1q_u32(t + i, g_pack4(r, g, b));
            }
        }
    }
}

/* ---- BLUR_FS ---- */

static const float bo[4] = { 0.f, 1.411764705882353f, 3.2941176470588234f, 5.176470588235294f };
static const float bw[4] = { .1964825501511404f, .2969069646728344f, .09447039785044732f, .010381362401148057f };

struct kern { int n; int off[16]; float w[16]; };
/* the pass's kernel along its axis: tap centre c0 = ratio * i + base, taps at c0 + d * bo[k] */
static void mk_kern(struct kern *k, float base, float d)
{
    k->n = 0;
    for (int t = -3; t <= 3; t++) {
        float c = base + d * (t < 0 ? -bo[-t] : bo[t]), wt = bw[t < 0 ? -t : t];
        int x0 = g_ifloor(c);
        float f = c - (float)x0;
        for (int e = 0; e < 2; e++) {
            int o = x0 + e;
            float ww = wt * (e ? f : 1.f - f);
            int j;
            for (j = 0; j < k->n && k->off[j] != o; j++)
                ;
            if (j == k->n) {
                k->off[j] = o;
                k->w[j] = 0.f;
                k->n++;
            }
            k->w[j] += ww;
        }
    }
}
static inline uint32_t q8(float r, float g, float b)
{
    return 0xff000000U | (uint32_t)(g_sat(r) * 255.f + .5f) << 16 | (uint32_t)(g_sat(g) * 255.f + .5f) << 8 |
           (uint32_t)(g_sat(b) * 255.f + .5f);
}
/* one renderWall blur(src, dst, dx, dy) pass; ratio = src / dst size (1 or 2). The same sums in the
 * same order as a tap-by-tap loop, but on float rows (planar, clamp-to-edge padding) so the inner
 * loops are straight multiply-adds. */
#define BPAD 16
static float BL[3][2 * WW + 2 * BPAD], BACC[3][WW];
static void blur_pass(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh, int horiz, float d)
{
    int ratio = sw / dw;
    struct kern k;
    /* centre along the axis for dst index 0: (0 + .5) * ratio - .5 */
    mk_kern(&k, .5f * (float)ratio - .5f, d);
    for (int j = 0; j < dh; j++) {
        if (!(j & 15))
            g7_poll();
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < dw; i++)
                BACC[c][i] = 0.f;
        if (horiz) {
            /* the source line (rows 2j, 2j + 1 at .5 each when downsampling), padded */
            const uint32_t *ra = src + (unsigned)(ratio * j) * sw, *rb = ratio == 2 ? ra + sw : ra;
            const float32x4_t half = vdupq_n_f32(.5f);
            for (int x = 0; x < sw; x += 8) {       /* sw is a multiple of 8 (720, 360) */
                float32x4_t u[3][2], v[3][2];
                g_unpack8(ra + x, u);
                g_unpack8(rb + x, v);
                for (int c = 0; c < 3; c++)
                    for (int h = 0; h < 2; h++)
                        vst1q_f32(BL[c] + BPAD + x + 4 * h, vmulq_f32(half, vaddq_f32(u[c][h], v[c][h])));
            }
            for (int x = -BPAD; x < 0; x++) {
                uint32_t a0 = ra[0], c0 = rb[0], a1 = ra[sw - 1], c1 = rb[sw - 1];
                BL[0][x + BPAD] = .5f * (float)(CR(a0) + CR(c0));
                BL[1][x + BPAD] = .5f * (float)(CG(a0) + CG(c0));
                BL[2][x + BPAD] = .5f * (float)(CB(a0) + CB(c0));
                BL[0][sw - 1 - x + BPAD] = .5f * (float)(CR(a1) + CR(c1));
                BL[1][sw - 1 - x + BPAD] = .5f * (float)(CG(a1) + CG(c1));
                BL[2][sw - 1 - x + BPAD] = .5f * (float)(CB(a1) + CB(c1));
            }
            for (int t = 0; t < k.n; t++) {
                float32x4_t w = vdupq_n_f32(k.w[t]);
                int o = k.off[t] + BPAD;
                for (int c = 0; c < 3; c++) {
                    const float *L = BL[c] + o;
                    float *A = BACC[c];
                    if (ratio == 2)
                        for (int i = 0; i < dw; i += 4)
                            vst1q_f32(A + i, vfmaq_f32(vld1q_f32(A + i), w, vld2q_f32(L + 2 * i).val[0]));
                    else
                        for (int i = 0; i < dw; i += 4)
                            vst1q_f32(A + i, vfmaq_f32(vld1q_f32(A + i), w, vld1q_f32(L + i)));
                }
            }
        } else {
            for (int t = 0; t < k.n; t++) {
                int y = ratio * j + k.off[t];
                y = y < 0 ? 0 : y >= sh ? sh - 1 : y;
                const uint32_t *r = src + (unsigned)y * sw;
                float32x4_t w = vdupq_n_f32(k.w[t]), wh = vdupq_n_f32(k.w[t] * .5f);
                if (ratio == 2) {
                    for (int i = 0; i < dw; i += 4) {
                        uint8x8x4_t p = vld4_u8((const uint8_t *)(r + 2 * i));
                        for (int c = 0; c < 3; c++) {
                            uint16x4_t sum = vpaddl_u8(p.val[2 - c]);       /* columns 2i + 2i+1 */
                            float32x4_t f = vcvtq_f32_u32(vmovl_u16(sum));
                            vst1q_f32(BACC[c] + i, vfmaq_f32(vld1q_f32(BACC[c] + i), wh, f));
                        }
                    }
                } else {
                    for (int i = 0; i < dw; i += 8) {
                        float32x4_t u[3][2];
                        g_unpack8(r + i, u);
                        for (int c = 0; c < 3; c++)
                            for (int h = 0; h < 2; h++)
                                vst1q_f32(BACC[c] + i + 4 * h, vfmaq_f32(vld1q_f32(BACC[c] + i + 4 * h), w, u[c][h]));
                    }
                }
            }
        }
        uint32_t *o = dst + (unsigned)j * dw;
        {
            /* q8(acc / 255): (sat(acc / 255) x 255 + .5), 4 pixels a step (dw: 360 or 180) */
            const float32x4_t z = vdupq_n_f32(0.f), mx = vdupq_n_f32(255.f), h = vdupq_n_f32(.5f);
            for (int i = 0; i < dw; i += 4) {
                float32x4_t r = vaddq_f32(vminq_f32(vmaxq_f32(vld1q_f32(BACC[0] + i), z), mx), h);
                float32x4_t g = vaddq_f32(vminq_f32(vmaxq_f32(vld1q_f32(BACC[1] + i), z), mx), h);
                float32x4_t b = vaddq_f32(vminq_f32(vmaxq_f32(vld1q_f32(BACC[2] + i), z), mx), h);
                vst1q_u32(o + i, g_pack4(r, g, b));
            }
        }
    }
}

void gw_blur(void)
{
    gw_planes();                    /* the canvas-grid frost planes are this blur's again */
    uint32_t *wall = TEX(P_TMP), *a1 = TEX(P_SOFT), *a2 = TEX(P_A), *b1 = TEX(P_SOFT + PL_A_BYTES), *b2 = TEX(P_B);
    /* renderWall: blur(wall,a1,2,0); blur(a1,a2,0,1.6); blur(a2,a1,1.6,0); blur(a1,a2,0,1.6);
     *             blur(a2,b1,2,0); blur(b1,b2,0,1.6); blur(b2,b1,1.6,0); blur(b1,b2,0,1.6) */
    blur_pass(wall, WW, WH, a1, AW, AH, 1, 2.f);
    blur_pass(a1, AW, AH, a2, AW, AH, 0, 1.6f);
    blur_pass(a2, AW, AH, a1, AW, AH, 1, 1.6f);
    blur_pass(a1, AW, AH, a2, AW, AH, 0, 1.6f);
    blur_pass(a2, AW, AH, b1, BW, BH, 1, 2.f);
    blur_pass(b1, BW, BH, b2, BW, BH, 0, 1.6f);
    blur_pass(b2, BW, BH, b1, BW, BH, 1, 1.6f);
    blur_pass(b1, BW, BH, b2, BW, BH, 0, 1.6f);
    /* frost(): A * .36 + the four diagonal taps at +-12 panel px = +-3 texels of A, * .16 */
    uint32_t *a5 = TEX(P_A5);
    for (int j = 0; j < AH; j++) {
        int jm = j - 3 < 0 ? 0 : j - 3, jp = j + 3 >= AH ? AH - 1 : j + 3;
        const uint32_t *rc = a2 + j * AW, *rm = a2 + jm * AW, *rp = a2 + jp * AW;
        const float32x4_t k36 = vdupq_n_f32(.36f / 255.f), k16 = vdupq_n_f32(.16f / 255.f), z = vdupq_n_f32(0.f),
                          o1 = vdupq_n_f32(1.f), k = vdupq_n_f32(1023.f), h = vdupq_n_f32(.5f);
        for (int i = 4; i + 4 <= AW - 4; i += 4) {      /* i - 3 >= 0, i + 3 + 3 < AW */
            float32x4_t c[3], t[4][3];
            g_unpack4(rc + i, c);
            g_unpack4(rm + i - 3, t[0]);
            g_unpack4(rm + i + 3, t[1]);
            g_unpack4(rp + i - 3, t[2]);
            g_unpack4(rp + i + 3, t[3]);
            uint32x4_t q[3];
            for (int ch = 0; ch < 3; ch++) {
                float32x4_t sum = vaddq_f32(vaddq_f32(t[0][ch], t[1][ch]), vaddq_f32(t[2][ch], t[3][ch]));
                float32x4_t v = vfmaq_f32(vmulq_f32(c[ch], k36), sum, k16);
                q[ch] = vcvtq_u32_f32(vfmaq_f32(h, vminq_f32(vmaxq_f32(v, z), o1), k));
            }
            vst1q_u32(a5 + j * AW + i, vorrq_u32(vorrq_u32(vshlq_n_u32(q[0], 20), vshlq_n_u32(q[1], 10)), q[2]));
        }
        for (int i = 0; i < AW; i++) {
            if (i == 4)
                i = AW - 4;     /* (4 .. AW - 4 above) */
            int im = i - 3 < 0 ? 0 : i - 3, ip = i + 3 >= AW ? AW - 1 : i + 3;
            uint32_t c = a2[j * AW + i], t0 = a2[jm * AW + im], t1 = a2[jm * AW + ip], t2 = a2[jp * AW + im],
                     t3 = a2[jp * AW + ip];
            float r = .36f * CR(c) + .16f * (float)(CR(t0) + CR(t1) + CR(t2) + CR(t3));
            float g = .36f * CG(c) + .16f * (float)(CG(t0) + CG(t1) + CG(t2) + CG(t3));
            float b = .36f * CB(c) + .16f * (float)(CB(t0) + CB(t1) + CB(t2) + CB(t3));
            a5[j * AW + i] = g_pack10(r * (1.f / 255.f), g * (1.f / 255.f), b * (1.f / 255.f));
        }
    }
}

/* ---- BASE: the wallpaper texture bilinear at panel pixel centres (texel x / 2 - .25) ---- */

static float WV[3][WW + 2];
/* the texture's two rows for panel row y blended (fy), texels t0 - 1 .. t1 + 1 clamped, into WV */
static void wall_vrow(const uint32_t *T, int y, int t0, int t1)
{
    float cy = (y + .5f) * .5f - .5f;
    int r0 = g_ifloor(cy);
    float fy = cy - (float)r0;
    int ra = r0 < 0 ? 0 : r0 >= WH ? WH - 1 : r0, rb = r0 + 1 >= WH ? WH - 1 : r0 + 1 < 0 ? 0 : r0 + 1;
    const uint32_t *A = T + (unsigned)ra * WW, *B = T + (unsigned)rb * WW;
    int t = t0;
    const float32x4_t vfy = vdupq_n_f32(fy), k = vdupq_n_f32(1.f / 255.f);
    for (; t <= t1; t++) {
        if (t >= 0 && t + 8 <= WW && t + 8 <= t1 + 1)
            break;
        int tc = t < 0 ? 0 : t >= WW ? WW - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        WV[0][t - t0] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
        WV[1][t - t0] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
        WV[2][t - t0] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
    }
    for (; t + 8 <= WW && t + 8 <= t1 + 1; t += 8) {
        float32x4_t u[3][2], v[3][2];
        g_unpack8(A + t, u);
        g_unpack8(B + t, v);
        for (int c = 0; c < 3; c++)
            for (int h = 0; h < 2; h++)
                vst1q_f32(WV[c] + (t - t0) + 4 * h, vmulq_f32(vfmaq_f32(u[c][h], vsubq_f32(v[c][h], u[c][h]), vfy), k));
    }
    for (; t <= t1; t++) {
        int tc = t < 0 ? 0 : t >= WW ? WW - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        WV[0][t - t0] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
        WV[1][t - t0] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
        WV[2][t - t0] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
    }
}
static void wall_row_panel(const uint32_t *T, int y, int x0, int x1, float *rgb)
{
    int t0 = g_ifloor((x0 + .5f) * .5f - .5f), t1 = g_ifloor((x1 - .5f) * .5f - .5f) + 1;
    wall_vrow(T, y, t0, t1);
    for (int x = x0; x < x1; x++) {
        int c0 = (x - 1) >> 1;                      /* floor(x / 2 - .25) */
        float fx = (x & 1) ? .25f : .75f;
        int k = c0 - t0;
        float *o = rgb + 3 * (x - x0);
        o[0] = WV[0][k] + (WV[0][k + 1] - WV[0][k]) * fx;
        o[1] = WV[1][k] + (WV[1][k + 1] - WV[1][k]) * fx;
        o[2] = WV[2][k] + (WV[2][k + 1] - WV[2][k]) * fx;
    }
}

/* the wallpaper at panel resolution x dim, dithered (GLASS_FS's store), into dst[x0 .. x1).
 * Panel x = 2m samples texels m - 1, m at .25 / .75, x = 2m + 1 texels m, m + 1 at .75 / .25: eight
 * pixels from five texels per NEON step. */
static inline uint32_t wall_px(int x, int t0, float k255, float Y)
{
    int k = ((x - 1) >> 1) - t0;
    float fx = (x & 1) ? .25f : .75f, h = g_dither(x, Y);
    float r = (WV[0][k] + (WV[0][k + 1] - WV[0][k]) * fx) * k255 + h;
    float g = (WV[1][k] + (WV[1][k + 1] - WV[1][k]) * fx) * k255 + h;
    float b = (WV[2][k] + (WV[2][k + 1] - WV[2][k]) * fx) * k255 + h;
    return 0xff000000U | g_q255(r) << 16 | g_q255(g) << 8 | g_q255(b);
}
void gw_row_wall8(int y, int x0, int x1, float dim, uint32_t *dst)
{
    const uint32_t *T = TEX(P_TMP);
    int t0 = g_ifloor((x0 + .5f) * .5f - .5f), t1 = g_ifloor((x1 - .5f) * .5f - .5f) + 1;
    wall_vrow(T, y, t0, t1);
    float Y = gs_dither_y(y), k255 = dim * 255.f;
    int x = x0;
    while (x < x1 && (x & 1))
        dst[x - x0] = wall_px(x, t0, k255, Y), x++;
    const float32x4_t q1 = vdupq_n_f32(.25f * k255), q3 = vdupq_n_f32(.75f * k255);
    for (; x + 8 <= x1; x += 8) {
        int k = (x >> 1) - 1 - t0;             /* texel m - 1 for x = 2m */
        uint32x4x2_t o;
        float32x4_t h0 = g_dither4(x, Y), h1 = g_dither4(x + 4, Y);
        float32x4_t c[3][2];
        for (int ch = 0; ch < 3; ch++) {
            float32x4_t a = vld1q_f32(WV[ch] + k), b = vld1q_f32(WV[ch] + k + 1), e = vld1q_f32(WV[ch] + k + 2);
            float32x4_t ev = vfmaq_f32(vmulq_f32(a, q1), b, q3);     /* x = 2m */
            float32x4_t od = vfmaq_f32(vmulq_f32(b, q3), e, q1);     /* x = 2m + 1 */
            float32x4x2_t z = vzipq_f32(ev, od);
            c[ch][0] = vaddq_f32(z.val[0], h0);
            c[ch][1] = vaddq_f32(z.val[1], h1);
        }
        o.val[0] = g_pack4(c[0][0], c[1][0], c[2][0]);
        o.val[1] = g_pack4(c[0][1], c[1][1], c[2][1]);
        vst1q_u32(dst + (x - x0), o.val[0]);
        vst1q_u32(dst + (x - x0) + 4, o.val[1]);
    }
    for (; x < x1; x++)
        dst[x - x0] = wall_px(x, t0, k255, Y);
}

/* a canvas row of the texture x dim, dithered (canvas pixel (i, j) is texel (i, j)): 8 bytes of
 * each channel widened, scaled in float, the dither added, 8 pixels a step */
void gw_crow_wall8(int j, int i0, int i1, float dim, uint32_t *dst)
{
    const uint32_t *T = TEX(P_TMP) + (unsigned)j * WW;
    float Y = gs_dither_y(j), k = dim;
    int i = i0;
    const float32x4_t vk = vdupq_n_f32(k);
    for (; i + 8 <= i1; i += 8) {
        float32x4_t c[3][2];
        g_unpack8(T + i, c);
        float32x4_t h0 = g_dither4(i, Y), h1 = g_dither4(i + 4, Y);
        vst1q_u32(dst + (i - i0), g_pack4(vfmaq_f32(h0, c[0][0], vk), vfmaq_f32(h0, c[1][0], vk), vfmaq_f32(h0, c[2][0], vk)));
        vst1q_u32(dst + (i - i0) + 4,
                  g_pack4(vfmaq_f32(h1, c[0][1], vk), vfmaq_f32(h1, c[1][1], vk), vfmaq_f32(h1, c[2][1], vk)));
    }
    for (; i < i1; i++) {
        uint32_t v = T[i];
        float h = g_dither(i, Y);
        dst[i - i0] = 0xff000000U | g_q255((float)CR(v) * k + h) << 16 | g_q255((float)CG(v) * k + h) << 8 |
                      g_q255((float)CB(v) * k + h);
    }
}

/* BG8 (G7_BASE): the final wallpaper x .8, dithered, what every pixel without glass shows (canvas) */
void gw_base(void)
{
    uint32_t *base = (uint32_t *)G7_BASE;
    for (int j = 0; j < CH; j++) {
        if (!(j & 31))
            g7_poll();
        gw_crow_wall8(j, 0, CW, .8f, base + (unsigned)j * CW);
    }
}

/* the wallpaper at panel resolution straight from the texture (intro frames, BASE not valid) */
void gw_row_wall(int y, int x0, int x1, float *rgb) { wall_row_panel(TEX(P_TMP), y, x0, x1, rgb); }

/* ---- bilinear samplers (WebGL: texel = uv * size - .5, clamp to edge) ---- */

static inline void bil8(const uint32_t *T, int w, int h, float cx, float cy, float out[3])
{
    int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
    float fx = cx - (float)x0, fy = cy - (float)y0;
    int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
    uint32_t p00 = T[ya * w + xa], p01 = T[ya * w + xb], p10 = T[yb * w + xa], p11 = T[yb * w + xb];
    float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
    out[0] = (w00 * CR(p00) + w01 * CR(p01) + w10 * CR(p10) + w11 * CR(p11)) * (1.f / 255.f);
    out[1] = (w00 * CG(p00) + w01 * CG(p01) + w10 * CG(p10) + w11 * CG(p11)) * (1.f / 255.f);
    out[2] = (w00 * CB(p00) + w01 * CB(p01) + w10 * CB(p10) + w11 * CB(p11)) * (1.f / 255.f);
}
static inline void bil10(const uint32_t *T, int w, int h, float cx, float cy, float out[3])
{
    int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
    float fx = cx - (float)x0, fy = cy - (float)y0;
    int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
    uint32_t p00 = T[ya * w + xa], p01 = T[ya * w + xb], p10 = T[yb * w + xa], p11 = T[yb * w + xb];
    float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
    out[0] = w00 * UNP10_R(p00) + w01 * UNP10_R(p01) + w10 * UNP10_R(p10) + w11 * UNP10_R(p11);
    out[1] = w00 * UNP10_G(p00) + w01 * UNP10_G(p01) + w10 * UNP10_G(p10) + w11 * UNP10_G(p11);
    out[2] = w00 * UNP10_B(p00) + w01 * UNP10_B(p01) + w10 * UNP10_B(p10) + w11 * UNP10_B(p11);
}
/* one panel row (py) of A5, A or B blended vertically, texels t0 .. t1 (clamped to the edge), planar
 * into out[c][t - t0]: the side spans' horizontal samples are then one lerp (glass_shade.c px_side) */
static void vrow_any(const uint32_t *T, int tw, int th, int pp, int ten, float py, int t0, int t1, float *o, int stride)
{
    float cy = py / (float)pp - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= th ? th - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= th ? th - 1 : y0 + 1;
    const uint32_t *A = T + ya * tw, *B = T + yb * tw;
    for (int t = t0; t <= t1; t++) {
        int tc = t < 0 ? 0 : t >= tw ? tw - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        int k = t - t0;
        if (ten) {
            o[k] = UNP10_R(a) + (UNP10_R(b) - UNP10_R(a)) * fy;
            o[stride + k] = UNP10_G(a) + (UNP10_G(b) - UNP10_G(a)) * fy;
            o[2 * stride + k] = UNP10_B(a) + (UNP10_B(b) - UNP10_B(a)) * fy;
        } else {
            o[k] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
            o[stride + k] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
            o[2 * stride + k] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
        }
    }
}
void gw_vrow_a5(float py, int t0, int t1, float *o, int stride) { vrow_any(TEX(P_A5), AW, AH, 4, 1, py, t0, t1, o, stride); }
void gw_vrow_a(float py, int t0, int t1, float *o, int stride) { vrow_any(TEX(P_A), AW, AH, 4, 0, py, t0, t1, o, stride); }
void gw_vrow_b(float py, int t0, int t1, float *o, int stride) { vrow_any(TEX(P_B), BW, BH, 8, 0, py, t0, t1, o, stride); }

void gw_sample_a(float px, float py, float out[3]) { bil8(TEX(P_A), AW, AH, px * .25f - .5f, py * .25f - .5f, out); }
void gw_sample_a5(float px, float py, float out[3]) { bil10(TEX(P_A5), AW, AH, px * .25f - .5f, py * .25f - .5f, out); }
void gw_sample_b(float px, float py, float out[3]) { bil8(TEX(P_B), BW, BH, px * .125f - .5f, py * .125f - .5f, out); }

/* rows at any spacing (half resolution: dpx 2): the two texel rows blended once (vrow), then one
 * lerp per sample */
static float vrow[3][AW + 4];
static void row_at(const uint32_t *T, int tw, int th, int pp, int ten, float py, int n, float px0, float dpx,
                   float *rgb)
{
    float ip = 1.f / (float)pp, cy = py * ip - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= th ? th - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= th ? th - 1 : y0 + 1;
    const uint32_t *A = T + ya * tw, *B = T + yb * tw;
    int t0 = g_ifloor(px0 * ip - .5f), t1 = g_ifloor((px0 + dpx * (float)(n - 1)) * ip - .5f) + 1;
    if (t1 - t0 > AW + 2)
        t1 = t0 + AW + 2;
    for (int t = t0; t <= t1; t++) {
        int tc = t < 0 ? 0 : t >= tw ? tw - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        int k = t - t0;
        if (ten) {
            vrow[0][k] = UNP10_R(a) + (UNP10_R(b) - UNP10_R(a)) * fy;
            vrow[1][k] = UNP10_G(a) + (UNP10_G(b) - UNP10_G(a)) * fy;
            vrow[2][k] = UNP10_B(a) + (UNP10_B(b) - UNP10_B(a)) * fy;
        } else {
            vrow[0][k] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
            vrow[1][k] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
            vrow[2][k] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
        }
    }
    for (int i = 0; i < n; i++) {
        float cx = (px0 + dpx * (float)i) * ip - .5f;
        int c0 = g_ifloor(cx);
        float fx = cx - (float)c0;
        int k = c0 - t0;
        if (k > t1 - t0 - 1)
            k = t1 - t0 - 1;
        float *o = rgb + 3 * i;
        o[0] = vrow[0][k] + (vrow[0][k + 1] - vrow[0][k]) * fx;
        o[1] = vrow[1][k] + (vrow[1][k + 1] - vrow[1][k]) * fx;
        o[2] = vrow[2][k] + (vrow[2][k + 1] - vrow[2][k]) * fx;
    }
}
/* the half-resolution grid (panel x = 2g + 1): the texel rows blended 4 texels a NEON step, then the
 * grid's fixed weights, 8 samples a step. A (pp 4): g = 2m at .75 between texels m - 1, m and
 * g = 2m + 1 at .25 between m, m + 1. B (pp 8): g = 4m + j at .625, .875 (m - 1, m), .125, .375 (m, m + 1). */
static float hrow[3][AW + 16];
static void row_half(const uint32_t *T, int tw, int th, int pp, int ten, float py, int n, int g0, float *rgb, int sh)
{
    float cy = py / (float)pp - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= th ? th - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= th ? th - 1 : y0 + 1;
    const uint32_t *A = T + ya * tw, *B = T + yb * tw;
    int t0 = (g0 >> sh) - 1, t1 = ((g0 + n - 1) >> sh) + 2;    /* texels t0 .. t1 */
    int nt = t1 - t0 + 1;
    const float32x4_t vfy = vdupq_n_f32(fy), k10 = vdupq_n_f32(1.f / 1023.f), k8 = vdupq_n_f32(1.f / 255.f);
    for (int k = 0; k < nt; k += 4) {
        uint32_t ta[4], tb[4];
        for (int j = 0; j < 4; j++) {
            int t = t0 + k + j, tc = t < 0 ? 0 : t >= tw ? tw - 1 : t;
            ta[j] = A[tc];
            tb[j] = B[tc];
        }
        uint32x4_t a = vld1q_u32(ta), b = vld1q_u32(tb);
        for (int c = 0; c < 3; c++) {
            float32x4_t va, vb;
            if (ten) {
                int32x4_t sv = vdupq_n_s32(-(20 - 10 * c));
                va = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshlq_u32(a, sv), vdupq_n_u32(1023U))), k10);
                vb = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshlq_u32(b, sv), vdupq_n_u32(1023U))), k10);
            } else {
                int32x4_t sv = vdupq_n_s32(-(16 - 8 * c));
                va = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshlq_u32(a, sv), vdupq_n_u32(255U))), k8);
                vb = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshlq_u32(b, sv), vdupq_n_u32(255U))), k8);
            }
            vst1q_f32(hrow[c] + k, vfmaq_f32(va, vsubq_f32(vb, va), vfy));
        }
    }
    /* sample g (= g0 + i) from texels (lo, lo + 1) at f */
    for (int i = 0; i < n; i++) {
        int g = g0 + i, m = g >> sh, j = g & ((1 << sh) - 1), lo;
        float f;
        if (sh == 1) {
            lo = j ? m : m - 1;
            f = j ? .25f : .75f;
        } else {
            lo = j < 2 ? m - 1 : m;
            f = j == 0 ? .625f : j == 1 ? .875f : j == 2 ? .125f : .375f;
        }
        int k = lo - t0;
        float *o = rgb + 3 * i;
        o[0] = hrow[0][k] + (hrow[0][k + 1] - hrow[0][k]) * f;
        o[1] = hrow[1][k] + (hrow[1][k + 1] - hrow[1][k]) * f;
        o[2] = hrow[2][k] + (hrow[2][k + 1] - hrow[2][k]) * f;
    }
}
/* A5 and B on the canvas grid (canvas pixel (i, j) = panel point (2i + 1, 2j + 1)), packed 10:10:10,
 * in the upper 8.5 MiB of G7_SHARP: a frost row of the canvas grid is a row read. Each row is built
 * the first time it is read (not in the bake: the bake's time decides v7 against v6). */
#define A5C ((uint32_t *)(G7_SHARP + 0x420000UL))
#define BCP ((uint32_t *)(G7_SHARP + 0x840000UL))
_Static_assert(0x840000UL + 4UL * CW * CH <= G7_BASE - G7_SHARP, "canvas frost planes overflow G7_SHARP");
static uint8_t PROW[2][CH];         /* the plane's row is built (gw_blur clears it) */
static void row_half(const uint32_t *T, int tw, int th, int pp, int ten, float py, int n, int g0, float *rgb, int sh);
static float PRT[3 * CW];
static void plane_build(int p, int j)
{
    if (p)
        row_half(TEX(P_B), BW, BH, 8, 0, 2.f * (float)j + 1.f, CW, 0, PRT, 2);
    else
        row_half(TEX(P_A5), AW, AH, 4, 1, 2.f * (float)j + 1.f, CW, 0, PRT, 1);
    uint32_t *d = (p ? BCP : A5C) + (unsigned)j * CW;
    const float32x4_t k = vdupq_n_f32(1023.f), h = vdupq_n_f32(.5f), z = vdupq_n_f32(0.f), o = vdupq_n_f32(1.f);
    for (int i = 0; i < CW; i += 4) {
        float32x4x3_t c = vld3q_f32(PRT + 3 * i);
        uint32x4_t r = vcvtq_u32_f32(vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[0], z), o), k));
        uint32x4_t g = vcvtq_u32_f32(vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[1], z), o), k));
        uint32x4_t b = vcvtq_u32_f32(vfmaq_f32(h, vminq_f32(vmaxq_f32(c.val[2], z), o), k));
        vst1q_u32(d + i, vorrq_u32(vorrq_u32(vshlq_n_u32(r, 20), vshlq_n_u32(g, 10)), b));
    }
    PROW[p][j] = 1;
}
void gw_planes(void)
{
    for (int j = 0; j < CH; j++)
        PROW[0][j] = PROW[1][j] = 0;
}
/* n samples of the canvas-grid plane p's row j from column g0, unpacked to rgb triplets */
static void plane_row(int p, int j, int g0, int n, float *rgb)
{
    if (!PROW[p][j])
        plane_build(p, j);
    const uint32_t *s = (p ? BCP : A5C) + (unsigned)j * CW + g0;
    const uint32x4_t m = vdupq_n_u32(1023U);
    const float32x4_t k = vdupq_n_f32(1.f / 1023.f);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t v = vld1q_u32(s + i);
        float32x4x3_t o;
        o.val[0] = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshrq_n_u32(v, 20), m)), k);
        o.val[1] = vmulq_f32(vcvtq_f32_u32(vandq_u32(vshrq_n_u32(v, 10), m)), k);
        o.val[2] = vmulq_f32(vcvtq_f32_u32(vandq_u32(v, m)), k);
        vst3q_f32(rgb + 3 * i, o);
    }
    for (; i < n; i++) {
        uint32_t v = s[i];
        rgb[3 * i] = UNP10_R(v);
        rgb[3 * i + 1] = UNP10_G(v);
        rgb[3 * i + 2] = UNP10_B(v);
    }
}
/* the row is a canvas row (py = 2j + 1) inside the canvas: the plane */
static inline int plane_at(float py, int g0, int n, int *j)
{
    if (g0 < 0 || g0 + n > CW)
        return 0;
    float c = (py - 1.f) * .5f;
    int ci = (int)c;
    if ((float)ci != c || ci < 0 || ci >= CH)
        return 0;
    *j = ci;
    return 1;
}
/* the quarter grid (panel x = 4g + 2): A5's texels themselves (4 px each), B at 2 samples a texel
 * (the half grid's pattern for A) */
static inline int quarter_grid(float px0, float dpx, int n, int *g0)
{
    if (dpx != 4.f || n > AW + 8)
        return 0;
    float g = (px0 - 2.f) * .25f;
    int gi = (int)g;
    if ((float)gi != g || gi < -4)
        return 0;
    *g0 = gi;
    return 1;
}
static void a5_quarter(float py, int n, int g0, float *rgb)
{
    float cy = py * .25f - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= AH ? AH - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= AH ? AH - 1 : y0 + 1;
    const uint32_t *A = TEX(P_A5) + ya * AW, *B = TEX(P_A5) + yb * AW;
    for (int i = 0; i < n; i++) {
        int t = g0 + i, tc = t < 0 ? 0 : t >= AW ? AW - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        rgb[3 * i] = UNP10_R(a) + (UNP10_R(b) - UNP10_R(a)) * fy;
        rgb[3 * i + 1] = UNP10_G(a) + (UNP10_G(b) - UNP10_G(a)) * fy;
        rgb[3 * i + 2] = UNP10_B(a) + (UNP10_B(b) - UNP10_B(a)) * fy;
    }
}
static inline int half_grid(float px0, float dpx, int n, int *g0)
{
    if (dpx != 2.f || n > 2 * AW + 8)
        return 0;
    float g = (px0 - 1.f) * .5f;
    int gi = (int)g;
    if ((float)gi != g || gi < -4)
        return 0;
    *g0 = gi;
    return 1;
}
void gw_row_a5_at(float py, int n, float px0, float dpx, float *rgb)
{
    int g0, j;
    if (half_grid(px0, dpx, n, &g0) && plane_at(py, g0, n, &j))
        plane_row(0, j, g0, n, rgb);
    else if (half_grid(px0, dpx, n, &g0))
        row_half(TEX(P_A5), AW, AH, 4, 1, py, n, g0, rgb, 1);
    else if (quarter_grid(px0, dpx, n, &g0))
        a5_quarter(py, n, g0, rgb);
    else
        row_at(TEX(P_A5), AW, AH, 4, 1, py, n, px0, dpx, rgb);
}
void gw_row_b_at(float py, int n, float px0, float dpx, float *rgb)
{
    int g0, j;
    if (half_grid(px0, dpx, n, &g0) && plane_at(py, g0, n, &j))
        plane_row(1, j, g0, n, rgb);
    else if (half_grid(px0, dpx, n, &g0))
        row_half(TEX(P_B), BW, BH, 8, 0, py, n, g0, rgb, 2);
    else if (quarter_grid(px0, dpx, n, &g0))
        row_half(TEX(P_B), BW, BH, 8, 0, py, n, g0, rgb, 1);
    else
        row_at(TEX(P_B), BW, BH, 8, 0, py, n, px0, dpx, rgb);
}
void gw_row_a_at(float py, int n, float px0, float dpx, float *rgb)
{
    int g0;
    if (half_grid(px0, dpx, n, &g0))
        row_half(TEX(P_A), AW, AH, 4, 0, py, n, g0, rgb, 1);
    else
        row_at(TEX(P_A), AW, AH, 4, 0, py, n, px0, dpx, rgb);
}
/* full-resolution rows: blend the two texel rows once, then the fixed horizontal pattern (A: 4
 * panel px per texel, B: 8). Pixel x = pp t + pp / 2 + j (j = 0 .. pp - 1) lies between texels t
 * and t + 1 at fx = (j + .5) / pp: one vector of weights per texel, 4 pixels a step. */
static void row_fast(const uint32_t *T, int tw, int th, int pp, int ten, float py, int x0, int x1, float *rgb)
{
    float cy = py / (float)pp - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= th ? th - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= th ? th - 1 : y0 + 1;
    const uint32_t *A = T + ya * tw, *B = T + yb * tw;
    int t0 = g_ifloor((x0 + .5f) / (float)pp - .5f), t1 = g_ifloor((x1 - .5f) / (float)pp - .5f) + 1;
    for (int t = t0; t <= t1; t++) {
        int tc = t < 0 ? 0 : t >= tw ? tw - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        int k = t - t0;
        if (ten) {
            vrow[0][k] = UNP10_R(a) + (UNP10_R(b) - UNP10_R(a)) * fy;
            vrow[1][k] = UNP10_G(a) + (UNP10_G(b) - UNP10_G(a)) * fy;
            vrow[2][k] = UNP10_B(a) + (UNP10_B(b) - UNP10_B(a)) * fy;
        } else {
            vrow[0][k] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
            vrow[1][k] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
            vrow[2][k] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
        }
    }
    int x = x0, half = pp >> 1;
    /* to the first pixel of a texel interval (x = pp t + pp / 2) */
    while (x < x1 && ((x - half) & (pp - 1))) {
        float cx = (x + .5f) / (float)pp - .5f;
        int c0 = g_ifloor(cx);
        float fx = cx - (float)c0;
        int k = c0 - t0;
        float *o = rgb + 3 * (x - x0);
        for (int c = 0; c < 3; c++)
            o[c] = vrow[c][k] + (vrow[c][k + 1] - vrow[c][k]) * fx;
        x++;
    }
    const float32x4_t w4 = { .125f, .375f, .625f, .875f }, w8a = { .0625f, .1875f, .3125f, .4375f },
                      w8b = { .5625f, .6875f, .8125f, .9375f };
    for (; x + pp <= x1; x += pp) {
        int k = (x - half) / pp - t0;
        float32x4x3_t o0, o1;
        for (int c = 0; c < 3; c++) {
            float32x4_t v = vdupq_n_f32(vrow[c][k]), dv = vdupq_n_f32(vrow[c][k + 1] - vrow[c][k]);
            if (pp == 4) {
                o0.val[c] = vfmaq_f32(v, dv, w4);
            } else {
                o0.val[c] = vfmaq_f32(v, dv, w8a);
                o1.val[c] = vfmaq_f32(v, dv, w8b);
            }
        }
        vst3q_f32(rgb + 3 * (x - x0), o0);
        if (pp == 8)
            vst3q_f32(rgb + 3 * (x - x0) + 12, o1);
    }
    for (; x < x1; x++) {
        float cx = (x + .5f) / (float)pp - .5f;
        int c0 = g_ifloor(cx);
        float fx = cx - (float)c0;
        int k = c0 - t0;
        float *o = rgb + 3 * (x - x0);
        for (int c = 0; c < 3; c++)
            o[c] = vrow[c][k] + (vrow[c][k + 1] - vrow[c][k]) * fx;
    }
}
void gw_row_a5(int y, int x0, int x1, float *rgb) { row_fast(TEX(P_A5), AW, AH, 4, 1, y + .5f, x0, x1, rgb); }
/* rows of A5, B, A at any panel y (pixel centres x0 + .5 ..): the straight rim bands' refracted rows */
void gw_row_a5_y(float py, int x0, int x1, float *rgb) { row_fast(TEX(P_A5), AW, AH, 4, 1, py, x0, x1, rgb); }
void gw_row_b_y(float py, int x0, int x1, float *rgb) { row_fast(TEX(P_B), BW, BH, 8, 0, py, x0, x1, rgb); }
void gw_row_a_y(float py, int x0, int x1, float *rgb) { row_fast(TEX(P_A), AW, AH, 4, 0, py, x0, x1, rgb); }
void gw_row_b(int y, int x0, int x1, float *rgb) { row_fast(TEX(P_B), BW, BH, 8, 0, y + .5f, x0, x1, rgb); }
