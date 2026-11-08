/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass: the concept's GLASS_FS (docs/concepts/ios27-boot.html), one row at a time.
 *
 * Per pixel and per shape, in layer order, exactly as the shader: the drop shadow (sdSq offset
 * 24, e^(-d/48), 32 %, smoothstep(-aa, 2 aa, d), culled at 110 / 150 px), coverage
 * clamp(.5 - d / aa), the bezel clamp(.8 r, 16, 64), the normal from central differences of the
 * SDF at 1.5 px where d > -bez - 4, shade() (frost from A5 / B, dispersion from A, vibrancy,
 * tint, milk, edge light, inner band, far side, specular), the lens rule, G7_S_CLIP's viewport
 * fade. Then the SAMSUNG glass wordmark (fld(), its gradient at 2.5 px) and the dither.
 *
 * sdSq's pow(): the continuous corner is homogeneous of degree 1, (mx^n + my^n)^(1/n) =
 * max * g(min / max) with g(t) = (1 + t^n)^(1/n), so a 1025-entry table per exponent (linear
 * interpolation: error < 1e-7 relative) replaces pow. Straight edges never reach the table.
 * The specular lobe exp(-2.2 |dq|^2) is separable: a column table per shape and a row factor.
 */
#include "glass_int.h"

#define GT_N 1024
#define NGT 15                     /* 15 x 1025 floats fit 0x40000 .. 0x50000 */
#define TAB_GT (P_TAB + 0x40000UL)                          /* 8 x 1025 floats */
#define TAB_EX (P_TAB + 0x50000UL)                          /* GS_NL sets x 20 shapes x 1440 floats */
#define EX_N 1440

struct gsh {
    float cx, cy, hbx, hby;         /* centre, half size */
    float r, eps2;                  /* sdSq's radius after the continuous-corner scale, 1e-3 * 2^(1/n) */
    const float *gt;                /* g(t) for n, 0 when n = 2 */
    float aa, bez, ibez;
    float alpha, s;
    float tr, tg, tb, tA;           /* tint rgb, clamp(T.a + SZ * .2) */
    float mixf, vib, milk;          /* frost toward B, saturation, .016 (1 - FR) */
    int lens, clip;
    float spx, spy, sdx, sdy;       /* specular centre and 1 / (hb * 1.05 + 50) */
    int ex0, exn;                   /* column table: grid index of entry 0, entries */
    float *ex;
    float gn;                       /* sdSq's exponent (gt is its table, gti the slot) */
    int gti;
};
/* gs_prepare's inputs per (layer, slot): a shape whose inputs equal the last call's keeps its
 * prepared entry (and its specular column table); one that only moved vertically keeps all but its
 * centre */
struct gkey {
    float x, y, w, h, r, a, s, t[4];
    float aa, soft, K, Ldx, Ldy;
    uint32_t flags;
    int valid;
};
static struct gkey GK[GS_NL][G7_MAX_SHAPES];
static struct gsh SH[GS_NL][G7_MAX_SHAPES];
static int NSH[GS_NL], GRID[GS_NL], NPRE[GS_NL];
static float LIT[GS_NL], RIMK[GS_NL];
static int OPQ[GS_NL];              /* the layer is opaque (base): al tracks 1 - M, M the multiplier of below */

/* g(t) tables by exponent: most shapes have n = 2.6 (r / min half size < .72) or 2 (capsules,
 * no table); the few in between (a lens changing shape) get a slot each, reused across frames */
static float gt_n[NGT];
static uint32_t gt_stamp[NGT], gt_frame = 1;
static int gt_used;

void gs_new_frame(void) { gt_frame++; }

static const float *gtable(float n, int *slot)
{
    float *base = (float *)TAB_GT;
    int best = -1;
    for (int i = 0; i < gt_used; i++)
        if (gt_n[i] == n) {
            gt_stamp[i] = gt_frame;
            *slot = i;
            return base + i * (GT_N + 1);
        }
    if (gt_used < NGT)
        best = gt_used++;
    else {
        /* evict a table no shape of this frame uses; if all are in use, take the nearest n */
        for (int i = 0; i < NGT && best < 0; i++)
            if (gt_stamp[i] != gt_frame)
                best = i;
        if (best < 0) {
            best = 0;
            for (int i = 1; i < NGT; i++)
                if (g_abs(gt_n[i] - n) < g_abs(gt_n[best] - n))
                    best = i;
            *slot = -1;             /* (borrowed: not this n) */
            return base + best * (GT_N + 1);
        }
    }
    float *t = base + best * (GT_N + 1);
    for (int k = 0; k <= GT_N; k++) {
        float x = (float)k / GT_N;
        t[k] = g_pow(1.f + g_pow(x, n), 1.f / n);
    }
    t[0] = 1.f;
    gt_n[best] = n;
    gt_stamp[best] = gt_frame;
    *slot = best;
    return t;
}

static inline float sdq(const struct gsh *s, float qx, float qy)
{
    float dx = g_abs(qx) - s->hbx + s->r, dy = g_abs(qy) - s->hby + s->r;
    if (dx <= 0.f) {
        if (dy <= 0.f)
            return g_max(dx, dy) - s->r + s->eps2;
        return dy + 1e-3f - s->r;
    }
    if (dy <= 0.f)
        return dx + 1e-3f - s->r;
    float mx = dx + 1e-3f, my = dy + 1e-3f;
    if (!s->gt)
        return g_sqrt(mx * mx + my * my) - s->r;
    float hi = g_max(mx, my), lo = g_min(mx, my);
    float t = lo / hi * (float)GT_N;
    int i = (int)t;
    if (i >= GT_N)
        i = GT_N - 1;
    float f = t - (float)i;
    return hi * (s->gt[i] + (s->gt[i + 1] - s->gt[i]) * f) - s->r;
}

/* sdSq's parameters for (b, r) */
static void sq_params(struct gsh *s, float r)
{
    float mb = g_min(s->hbx, s->hby);
    float k = g_smooth(.72f, .98f, mb > 0.f ? r / mb : 1.f);
    float n = g_mix(2.6f, 2.f, k);
    s->r = g_min(r * g_mix(1.18f, 1.f, k), mb);
    s->gn = n;
    s->gti = -1;
    s->gt = n == 2.f ? 0 : gtable(n, &s->gti);
    s->eps2 = 1e-3f * g_exp2(1.f / n);
}

/* a cached entry's g(t) table: still its exponent's (another shape may have taken the slot) */
static void gt_keep(struct gsh *s)
{
    if (!s->gt)
        return;
    if (s->gti >= 0 && gt_n[s->gti] == s->gn) {
        gt_stamp[s->gti] = gt_frame;
        return;
    }
    s->gt = gtable(s->gn, &s->gti);
}

void gs_prepare(int layer, const struct g7_shape *in, unsigned n, const struct gs_ctx *c, float zoom)
{
    /* the soft base (freeze) and the motion sets: the quarter grid */
    int G = layer == GS_SOFT || layer >= GS_QB ? 4 : layer > GS_SOFT ? 2 : 1;
    /* the half-resolution grid at the panel's uniforms takes the concept's own canvas scale (K 2,
     * its canvas runs at .4625 of the panel): aa 2.5 px, the rim falloff 2.2 px, one grid step of
     * anti-aliasing, so the bilinear upsample has no stairs */
    float K = G >= 2 && c->K < 2.f ? 2.f : c->K;
    if (layer >= GS_QB && K < 4.f)
        K = 4.f;                    /* one quarter-grid step of anti-aliasing: no stairs in the 2x */
    GRID[layer] = G;
    LIT[layer] = 1.f - c->soft * .85f;
    RIMK[layer] = g_max(2.f, K * 1.1f);
    OPQ[layer] = c->opaque;
    float aa = K * 1.25f + c->soft * 30.f;
    int m = 0, lens_at = G7_MAX_SHAPES;
    for (unsigned i = 0; i < n && i < G7_MAX_SHAPES; i++)
        if (in[i].flags & G7_S_LENS) {
            lens_at = (int)i;
            break;
        }
    NPRE[layer] = 0;
    for (unsigned i = 0; i < n && i < G7_MAX_SHAPES; i++) {
        const struct g7_shape *p = &in[i];
        float x = p->x, y = p->y, w = p->w, h = p->h, r = p->r;
        if (zoom != 1.f) {          /* #menu's scale about (720, 1332) */
            x = 720.f + (x - 720.f) * zoom;
            y = 1332.f + (y - 1332.f) * zoom;
            w *= zoom;
            h *= zoom;
            r *= zoom;
        }
        if (p->a <= .002f || w < 1.f || h < 1.f)
            continue;
        struct gsh *s = &SH[layer][m];
        struct gkey *k = &GK[layer][m];
        if ((int)i < lens_at)
            NPRE[layer] = m + 1;
        /* the cache: everything but y equal -> only the centre moves; x, w and the light equal ->
         * the specular column table stays */
        int same = k->valid && k->w == w && k->h == h && k->r == r && k->a == p->a && k->s == p->s &&
                   k->flags == p->flags && k->t[0] == p->tint[0] && k->t[1] == p->tint[1] && k->t[2] == p->tint[2] &&
                   k->t[3] == p->tint[3] && k->aa == aa && k->soft == c->soft && k->K == K && k->Ldy == c->Ldy;
        int same_ex = same && k->x == x && k->Ldx == c->Ldx;
        if (same_ex) {
            gt_keep(s);
            if (k->y != y) {
                s->cy = y + s->hby;
                s->spy = s->cy + c->Ldy * s->hby * .6f;
                k->y = y;
            }
            m++;
            continue;
        }
        k->valid = 1;
        k->x = x;
        k->y = y;
        k->w = w;
        k->h = h;
        k->r = r;
        k->a = p->a;
        k->s = p->s;
        k->flags = p->flags;
        for (int q = 0; q < 4; q++)
            k->t[q] = p->tint[q];
        k->aa = aa;
        k->soft = c->soft;
        k->K = K;
        k->Ldx = c->Ldx;
        k->Ldy = c->Ldy;
        s->hbx = w * .5f;
        s->hby = h * .5f;
        s->cx = x + s->hbx;
        s->cy = y + s->hby;
        sq_params(s, r);
        s->aa = aa;
        s->bez = g_clamp(r * .8f, 16.f, 64.f);
        s->ibez = 1.f / s->bez;
        s->alpha = p->a;
        s->lens = (p->flags & G7_S_LENS) != 0;
        s->clip = (p->flags & G7_S_CLIP) != 0;
        s->s = p->s;
        float FR = (p->flags & G7_S_FROST) ? 1.f : 0.f;
        float SZ = s->lens ? 0.f : g_smooth(300.f, 900.f, g_min(w, h));
        s->tr = p->tint[0];
        s->tg = p->tint[1];
        s->tb = p->tint[2];
        s->tA = g_sat(p->tint[3] + SZ * .2f);
        s->mixf = g_max(g_max(c->soft * .8f, SZ * .75f), FR * .92f);
        s->vib = g_mix(1.45f, 1.2f, FR);
        s->milk = .016f * (1.f - FR);
        s->spx = s->cx + c->Ldx * s->hbx * .6f;
        s->spy = s->cy + c->Ldy * s->hby * .6f;
        s->sdx = 1.f / (s->hbx * 1.05f + 50.f);
        s->sdy = 1.f / (s->hby * 1.05f + 50.f);
        /* specular column table over the grid columns of the shape box (+ aa) */
        float xa = s->cx - s->hbx - aa - 2.f, xb = s->cx + s->hbx + aa + 2.f;
        int gw = GW / G;
        int i0 = g_ifloor(xa / (float)G - .5f), i1 = g_ifloor(xb / (float)G - .5f) + 1;
        /* the half-resolution rows' grid runs from column -1 to gw (half_row: ia = (x0 - 1) >> 1,
         * through ib = gw), and px_interior / px_band read the table unchecked: cover those too
         * (column -1 read the float before this shape's entries, garbage at boot) */
        if (i0 < 1 - G)
            i0 = 1 - G;
        if (i1 > gw + G - 1)
            i1 = gw + G - 1;
        if (i1 - i0 > EX_N)
            i1 = i0 + EX_N;
        s->ex0 = i0;
        s->exn = i1 > i0 && G > 1 ? i1 - i0 : 0;      /* (G 1: never shaded) */
        unsigned slot = layer == GS_QB ? GS_BASE : layer == GS_QO ? GS_OVER : (unsigned)layer;
        s->ex = (float *)TAB_EX + (slot * G7_MAX_SHAPES + (unsigned)m) * EX_N;
        for (int q = 0; q < s->exn; q++) {
            float px = ((float)(i0 + q) + .5f) * (float)G, dq = (px - s->spx) * s->sdx;
            s->ex[q] = g_exp(-2.2f * dq * dq);
        }
        m++;
    }
    NSH[layer] = m;
}

/* frost(): A5 toward B by mixf, at a panel point */
static inline void frost_at(float px, float py, float mixf, float col[3])
{
    float a[3], b[3];
    gw_sample_a5(px, py, a);
    gw_sample_b(px, py, b);
    col[0] = a[0] + (b[0] - a[0]) * mixf;
    col[1] = a[1] + (b[1] - a[1]) * mixf;
    col[2] = a[2] + (b[2] - a[2]) * mixf;
}

/* shade(): col from the frost sample (already in col), then everything after it */
static inline void shade_tail(float col[3], float d, float nx, float ny, float bend, float tr, float tg, float tb,
                              float tA, float vib, float milk, float lit, float rimk, float Ldx, float Ldy, float spec)
{
    float L = .2126f * col[0] + .7152f * col[1] + .0722f * col[2];
    float r = (L + (col[0] - L) * vib) * 1.1f + .014f;
    float g = (L + (col[1] - L) * vib) * 1.1f + .014f;
    float b = (L + (col[2] - L) * vib) * 1.1f + .014f;
    float k = .42f + L * 1.3f;
    r += (tr * k - r) * tA;
    g += (tg * k - g) * tA;
    b += (tb * k - b) * tA;
    float mk = milk * (1.f - bend);
    r += mk;
    g += mk;
    b += mk;
    float f = nx * Ldx + ny * Ldy, fp = g_max(f, 0.f), fn = g_max(-f, 0.f);
    if (d > -30.f) {
        float rim = g_exp(g_min(d, 0.f) / rimk) * (.07f + .6f * fp * fp + .32f * fn * fn) * lit;
        r += rim;
        g += rim;
        b += rim;
    }
    if (bend > 0.f) {
        float band = bend * (.03f + .08f * fp) * lit, far = 1.f - bend * .14f * fn;
        r = (r + band) * far;
        g = (g + band) * far;
        b = (b + band) * far;
    }
    float sp = spec * .045f * lit;
    col[0] = r + sp;
    col[1] = g + sp;
    col[2] = b + sp;
}

/* the panel x range [x0, x1) the layer's shapes touch on the row at py (their 110 px cull); 0: none */
int gs_row_span(int layer, float py, int s0, int s1, int *x0, int *x1)
{
    float a = 1e9f, b = -1e9f;
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    if (s1 > NSH[layer])
        s1 = NSH[layer];
    for (int si = s0; si < s1; si++) {
        const struct gsh *s = &SH[layer][si];
        if (g_abs(py - s->cy) > s->hby + 150.f)
            continue;
        if ((s->clip ? s->alpha * fade : s->alpha) < .002f)
            continue;
        a = g_min(a, s->cx - s->hbx - 110.f);
        b = g_max(b, s->cx + s->hbx + 110.f);
    }
    if (a > b)
        return 0;
    *x0 = g_ifloor(a) - 1;
    *x1 = (int)__builtin_ceilf(b) + 1;
    return 1;
}

int gs_npre(int layer) { return NPRE[layer]; }

/* which prepared shapes s < s1 have py inside their box (bit s), as gs_classes picks them */
uint32_t gs_class_mask(int layer, float py, int s1)
{
    uint32_t m = 0;
    if (s1 > NSH[layer])
        s1 = NSH[layer];
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    for (int si = 0; si < s1; si++) {
        const struct gsh *s = &SH[layer][si];
        if (g_abs(py - s->cy) >= s->hby || (s->clip ? s->alpha * fade : s->alpha) < .002f || s->lens)
            continue;
        m |= 1U << si;
    }
    return m;
}

/* The moving-frame frost correction (glass.c glb_shift_row): on the row at py, the x spans of the
 * prepared shapes s < s1 whose box holds py, the later shape winning where they overlap, each with
 * the linear part of shade() (vibrancy, tint) as a 3 x 3 matrix and its frost mix toward B. */
int gs_classes(int layer, float py, int s1, struct gs_cls *o, int max)
{
    int n = 0;
    if (s1 > NSH[layer])
        s1 = NSH[layer];
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    static const float w[3] = { .2126f, .7152f, .0722f };
    for (int si = 0; si < s1; si++) {
        const struct gsh *s = &SH[layer][si];
        if (g_abs(py - s->cy) >= s->hby || (s->clip ? s->alpha * fade : s->alpha) < .002f || s->lens)
            continue;
        int a = (int)__builtin_ceilf(s->cx - s->hbx - .5f), b = (int)__builtin_ceilf(s->cx + s->hbx - .5f);
        if (a < 0) a = 0;
        if (b > GW) b = GW;
        if (a >= b)
            continue;
        struct gs_cls c;
        c.has_u = 0;
        for (int k = 0; k < n; k++)
            if (o[k].x0 <= a && o[k].x1 >= b) {
                c.has_u = 1;
                c.umixf = o[k].mixf;
                for (int j = 0; j < 9; j++)
                    c.uJ[j] = o[k].J[j];
                break;
            }
        /* cut [a, b) out of the spans so far, then insert it in order */
        static struct gs_cls keep[2 * G7_MAX_SHAPES + 2];      /* not on the 8 KiB stack */
        int m = 0;
        for (int k = 0; k < n && m + 2 <= (int)(sizeof keep / sizeof keep[0]); k++) {
            if (o[k].x1 <= a || o[k].x0 >= b) {
                keep[m++] = o[k];
                continue;
            }
            if (o[k].x0 < a) {
                keep[m] = o[k];
                keep[m++].x1 = a;
            }
            if (o[k].x1 > b) {
                keep[m] = o[k];
                keep[m++].x0 = b;
            }
        }
        c.si = si;
        c.x0 = a;
        c.x1 = b;
        c.mixf = s->mixf;
        /* a top / bottom rim band row: the straight part's refraction offset (as px_band) */
        c.oy = 0.f;
        c.bx0 = c.bx1 = 0;
        {
            const float h = 1.5f;
            float qy = py - s->cy, ddy = g_abs(qy) - s->hby + s->r, lim;
            if (ddy - h > 0.f)
                lim = -h;
            else if (ddy + h <= 0.f)
                lim = g_min(-h, ddy - 2.f * h - 1e-3f);
            else
                lim = -1e9f;
            float d = ddy > 0.f ? ddy + 1e-3f - s->r : ddy - s->r + s->eps2;
            if (lim > -1e8f && d > -s->bez - 4.f && !s->lens) {
                float e = g_sat(1.f + d * s->ibez), bend = 1.f - g_sqrt(g_max(1.f - e * e, 0.f));
                float X = s->hbx - s->r + lim;
                if (bend > 0.f && X > 0.f) {
                    c.oy = (qy < 0.f ? -1.f : 1.f) * bend * s->bez * 1.3f * s->s;
                    c.bx0 = (int)__builtin_ceilf(s->cx - X - .5f);
                    c.bx1 = (int)__builtin_ceilf(s->cx + X - .5f);
                }
            }
        }
        float t[3] = { s->tr, s->tg, s->tb }, k1 = (1.f - s->tA) * 1.1f;
        for (int r = 0; r < 3; r++)
            for (int j = 0; j < 3; j++)
                c.J[3 * r + j] = k1 * ((r == j ? s->vib : 0.f) + (1.f - s->vib) * w[j]) + 1.3f * s->tA * t[r] * w[j];
        n = 0;
        int put = 0;
        for (int k = 0; k < m && n < max; k++) {
            if (!put && keep[k].x0 >= b) {
                o[n++] = c;
                put = 1;
                if (n >= max)
                    break;
            }
            o[n++] = keep[k];
        }
        if (!put && n < max)
            o[n++] = c;
    }
    return n;
}
int gs_cover_row(int layer, int si, float py, int x0, int x1, float *cov)
{
    const struct gsh *s = &SH[layer][si];
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    float al = s->clip ? s->alpha * fade : s->alpha, iaa = 1.f / s->aa, qy = py - s->cy;
    float ay = g_abs(qy);
    int edge = ay > s->hby - 2.f * s->aa - 1.f;              /* the top / bottom edge: every pixel */
    int corner = ay > s->hby - s->r - 1.f;
    /* pixels with |qx| <= in are inside by more than the AA width */
    float in = edge ? -1.f : corner ? s->hbx - s->r - 1.f : s->hbx - 2.f * s->aa - 1.f;
    for (int x = x0; x < x1; x++) {
        float qx = (float)x + .5f - s->cx;
        cov[x - x0] = g_abs(qx) <= in ? al : g_sat(.5f - sdq(s, qx, qy) * iaa) * al;
    }
    return corner || edge;
}
int gs_cover_row_g(int layer, int si, float py, float px0, float dpx, int n, float *cov)
{
    const struct gsh *s = &SH[layer][si];
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    float al = s->clip ? s->alpha * fade : s->alpha, iaa = 1.f / s->aa, qy = py - s->cy;
    float ay = g_abs(qy);
    int edge = ay > s->hby - 2.f * s->aa - 1.f;
    int corner = ay > s->hby - s->r - 1.f;
    float in = edge ? -1.f : corner ? s->hbx - s->r - 1.f : s->hbx - 2.f * s->aa - 1.f;
    for (int k = 0; k < n; k++) {
        float qx = px0 + (float)k * dpx - s->cx;
        cov[k] = g_abs(qx) <= in ? al : g_sat(.5f - sdq(s, qx, qy) * iaa) * al;
    }
    return corner || edge;
}
int gs_nshapes(int layer) { return NSH[layer]; }

/* One shape's row, the per-pixel GLASS_FS for pixels i0 .. i1 - 1 (any class). */
struct rowc {                       /* what every pixel of one shape's row shares */
    const struct gsh *s;
    float py, qy, px0, alpha, aa, iaa, bez, ibez, ey, deep, k13, k8, omx, lit, rimk, Ldx, Ldy;
    int G, gx0;
    int mop;                        /* an opaque layer: a lens keeps al = 1 - M, M its multiplier of what is
                                     * below (the lens rule mixes it in at 1.07), so a layer drawn over
                                     * transparent black (C, M) composites as C + M x below */
};
static inline float lens_al(float al, float cov, float k) { return 1.f - (1.f - al) * (1.f - cov + 1.07f * k * cov); }
static void px_general(const struct rowc *R, int i0, int i1, float *acc, float *al, const float *rowa5,
                       const float *rowb)
{
    const struct gsh *s = R->s;
    float qy = R->qy, aa = R->aa, iaa = R->iaa, ibez = R->ibez, alpha = R->alpha;
    for (int i = i0; i < i1; i++) {
        float px = R->px0 + (float)(i * R->G);
        float qx = px - s->cx;
        float d = sdq(s, qx, qy);
        float *A = acc + 3 * i;
        if (!s->lens && d > -aa) {
            float sd2 = sdq(s, qx, qy - 24.f);
            float sh = .32f * alpha * g_exp(-g_max(sd2, 0.f) * (1.f / 48.f)) * g_smooth(-aa, aa * 2.f, d);
            float k = 1.f - sh;
            A[0] *= k;
            A[1] *= k;
            A[2] *= k;
            al[i] += sh * (1.f - al[i]);
        }
        if (d > aa)
            continue;
        float cov = g_sat(.5f - d * iaa) * alpha;
        int ei = R->gx0 + i - s->ex0;
        float spec = (ei >= 0 && ei < s->exn ? s->ex[ei] : 0.f) * R->ey;
        float col[3], e, bend, nx = 0.f, ny = 0.f;
        if (d <= R->deep) {         /* interior: n = 0, bend = 0, no dispersion */
            const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
            col[0] = fa[0] * R->omx + fb[0] * s->mixf;
            col[1] = fa[1] * R->omx + fb[1] * s->mixf;
            col[2] = fa[2] * R->omx + fb[2] * s->mixf;
            e = 0.f;
            bend = 0.f;
        } else {
            const float h = 1.5f;
            /* straight edges: all four samples on one side's straight part -> the axis */
            float ddx = g_abs(qx) - s->hbx + s->r, ddy = g_abs(qy) - s->hby + s->r;
            if (ddy + h <= 0.f && ddx - h > 0.f) {
                nx = qx < 0.f ? -1.f : 1.f;
            } else if (ddx + h <= 0.f && ddy - h > 0.f) {
                ny = qy < 0.f ? -1.f : 1.f;
            } else if (ddx + h <= 0.f && ddy + h <= 0.f && g_abs(ddx - ddy) > 2.f * h) {
                if (ddx > ddy)
                    nx = qx < 0.f ? -1.f : 1.f;
                else
                    ny = qy < 0.f ? -1.f : 1.f;
            } else {
                nx = sdq(s, qx + h, qy) - sdq(s, qx - h, qy);
                ny = sdq(s, qx, qy + h) - sdq(s, qx, qy - h);
                float ln = g_sqrt(nx * nx + ny * ny);
                ln = 1.f / g_max(ln, 1e-5f);
                nx *= ln;
                ny *= ln;
            }
            e = g_sat(1.f + d * ibez);
            bend = 1.f - g_sqrt(g_max(1.f - e * e, 0.f));
            float ox = nx * bend * R->k13, oy = ny * bend * R->k13;
            if (bend > 0.f) {
                frost_at(px + ox, R->py + oy, s->mixf, col);
                float dx = nx * bend * R->k8, dy = ny * bend * R->k8, t[3];
                gw_sample_a(px + ox + dx, R->py + oy + dy, t);
                col[0] += (t[0] - col[0]) * bend;
                gw_sample_a(px + ox - dx, R->py + oy - dy, t);
                col[2] += (t[2] - col[2]) * bend;
            } else {
                const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
                col[0] = fa[0] * R->omx + fb[0] * s->mixf;
                col[1] = fa[1] * R->omx + fb[1] * s->mixf;
                col[2] = fa[2] * R->omx + fb[2] * s->mixf;
            }
        }
        shade_tail(col, d, nx, ny, bend, s->tr, s->tg, s->tb, s->tA, s->vib, s->milk, R->lit, R->rimk, R->Ldx, R->Ldy,
                   spec);
        float kl = 0.f;
        if (s->lens) {
            kl = (1.f - e) * .82f;
            col[0] += (A[0] * 1.07f + .018f - col[0]) * kl;
            col[1] += (A[1] * 1.07f + .018f - col[1]) * kl;
            col[2] += (A[2] * 1.07f + .018f - col[2]) * kl;
        }
        A[0] += (col[0] - A[0]) * cov;
        A[1] += (col[1] - A[1]) * cov;
        A[2] += (col[2] - A[2]) * cov;
        al[i] = s->lens && R->mop ? lens_al(al[i], cov, kl) : al[i] + cov * (1.f - al[i]);
    }
}

/* outside the shape (d > 2 aa, so smoothstep(-aa, 2 aa, d) = 1): only the drop shadow,
 * .32 alpha e^(-max(sd2, 0) / 48) with sd2 = sdSq(p - (0, 24)). Where the offset box's straight part
 * faces the pixel sd2 is one coordinate: above / below it a constant over the row, beside it
 * |qx| - hbx + 1e-3, so e^(-sd2 / 48) is a geometric sequence along the row. */
static inline void shade_k(float *A, float *al, float sh)
{
    float k = 1.f - sh;
    A[0] *= k;
    A[1] *= k;
    A[2] *= k;
    *al += sh * (1.f - *al);
}
static void px_shadow(const struct rowc *R, int i0, int i1, float *acc, float *al)
{
    if (i0 >= i1)
        return;
    const struct gsh *s = R->s;
    float qs = R->qy - 24.f, k32 = .32f * R->alpha, G = (float)R->G;
    float ddy = g_abs(qs) - s->hby + s->r;
    /* the straight x part: |qx| <= hbx - r */
    float xa = s->cx - (s->hbx - s->r), xb = s->cx + (s->hbx - s->r);
    int ja = (int)__builtin_ceilf((xa - R->px0) / G), jb = g_ifloor((xb - R->px0) / G) + 1;  /* [ja, jb) */
    int i = i0;
    if (ddy > 0.f) {
        /* above / below the offset box: corners on both sides, the constant in the middle */
        float sh = k32 * g_exp(-g_max(ddy + 1e-3f - s->r, 0.f) * (1.f / 48.f));
        for (; i < i1; i++) {
            if (i >= ja && i < jb) {
                int e = jb < i1 ? jb : i1;
                float k = 1.f - sh;
                for (; i < e; i++) {
                    float *A = acc + 3 * i;
                    A[0] *= k;
                    A[1] *= k;
                    A[2] *= k;
                    al[i] += sh * (1.f - al[i]);
                }
                i--;
                continue;
            }
            float qx = R->px0 + (float)i * G - s->cx;
            shade_k(acc + 3 * i, al + i, k32 * g_exp(-g_max(sdq(s, qx, qs), 0.f) * (1.f / 48.f)));
        }
        return;
    }
    /* beside the offset box: |qx| > hbx - r on both sides of the shape (the shadow-only pixels are
     * outside it), sd2 = |qx| - hbx + 1e-3 once past the corner radius, i.e. outside [cx - hbx, cx + hbx] */
    float xl = s->cx - s->hbx, xr = s->cx + s->hbx;
    float step = g_exp(-G * (1.f / 48.f));
    int run = 0;
    float ev = 0.f;
    for (; i < i1; i++) {
        float px = R->px0 + (float)i * G, qx = px - s->cx;
        float sd2;
        if (px >= xr || px <= xl) {
            sd2 = g_abs(qx) - s->hbx + 1e-3f;
            /* right side: sd2 grows by G per pixel (e *= step); left side: shrinks (recompute) */
            if (px >= xr && run) {
                ev *= step;
            } else {
                ev = g_exp(-g_max(sd2, 0.f) * (1.f / 48.f));
                run = px >= xr;
            }
            shade_k(acc + 3 * i, al + i, k32 * ev);
            continue;
        }
        run = 0;
        sd2 = sdq(s, qx, qs);
        shade_k(acc + 3 * i, al + i, k32 * g_exp(-g_max(sd2, 0.f) * (1.f / 48.f)));
    }
}

/* the interior (d <= deep, d <= -30, d <= -aa: no shadow, cov = alpha, n = 0, bend = 0, no rim
 * light): shade_tail's linear part on the frost, the specular, the lens rule (e = 0), 4 pixels a step */
static void px_interior(const struct rowc *R, int i0, int i1, float *acc, float *al, const float *rowa5,
                        const float *rowb)
{
    const struct gsh *s = R->s;
    int i = i0;
    float vib = s->vib, tA = s->tA, alpha = R->alpha, spk = .045f * R->lit * R->ey;
    float32x4_t vomx = vdupq_n_f32(R->omx), vmix = vdupq_n_f32(s->mixf), vvib = vdupq_n_f32(vib);
    float32x4_t lr = vdupq_n_f32(.2126f), lg = vdupq_n_f32(.7152f), lb = vdupq_n_f32(.0722f);
    float32x4_t k11 = vdupq_n_f32(1.1f), k014 = vdupq_n_f32(.014f), k42 = vdupq_n_f32(.42f), k13 = vdupq_n_f32(1.3f);
    float32x4_t vtA = vdupq_n_f32(tA), vmilk = vdupq_n_f32(s->milk), vsp = vdupq_n_f32(spk), va = vdupq_n_f32(alpha);
    float32x4_t vt[3] = { vdupq_n_f32(s->tr), vdupq_n_f32(s->tg), vdupq_n_f32(s->tb) };
    float32x4_t one = vdupq_n_f32(1.f), k107 = vdupq_n_f32(1.07f), k018 = vdupq_n_f32(.018f), k82 = vdupq_n_f32(.82f);
    const float *ex = s->ex + (R->gx0 - s->ex0);
    for (; i + 4 <= i1; i += 4) {
        float32x4x3_t fa = vld3q_f32(rowa5 + 3 * i), fb = vld3q_f32(rowb + 3 * i), A = vld3q_f32(acc + 3 * i);
        float32x4_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = vfmaq_f32(vmulq_f32(fa.val[k], vomx), fb.val[k], vmix);
        float32x4_t L = vfmaq_f32(vfmaq_f32(vmulq_f32(c[0], lr), c[1], lg), c[2], lb);
        float32x4_t kk = vfmaq_f32(k42, L, k13);
        float32x4_t sp = vmulq_f32(vld1q_f32(ex + i), vsp);
        for (int k = 0; k < 3; k++) {
            float32x4_t r = vfmaq_f32(k014, vfmaq_f32(L, vsubq_f32(c[k], L), vvib), k11);
            r = vfmaq_f32(r, vsubq_f32(vmulq_f32(vt[k], kk), r), vtA);
            r = vaddq_f32(vaddq_f32(r, vmilk), sp);
            if (s->lens)
                r = vfmaq_f32(r, vsubq_f32(vfmaq_f32(k018, A.val[k], k107), r), k82);
            A.val[k] = vfmaq_f32(A.val[k], vsubq_f32(r, A.val[k]), va);
        }
        vst3q_f32(acc + 3 * i, A);
        float32x4_t l = vld1q_f32(al + i);
        if (s->lens && R->mop)      /* 1 - (1 - l)(1 - a + 1.07 x .82 a) */
            vst1q_f32(al + i, vsubq_f32(one, vmulq_f32(vsubq_f32(one, l), vfmaq_f32(vsubq_f32(one, va), va, vdupq_n_f32(1.07f * .82f)))));
        else
            vst1q_f32(al + i, vfmaq_f32(l, va, vsubq_f32(one, l)));
    }
    if (i < i1)
        px_general(R, i, i1, acc, al, rowa5, rowb);
}

/* A straight top or bottom rim band: pixels where GLASS_FS's normal is (0, +-1) and d the same for
 * the whole row (|qx| <= hbx - r - 1.5, and the row past the corner centre or well inside it). The
 * refracted samples are then rows: frost at y + oy, dispersion at y + oy +- dy, 4 pixels a step. */
static float BF5[3 * GW], BFB[3 * GW], BAP[3 * GW], BAM[3 * GW];
static void px_band(const struct rowc *R, int i0, int i1, float *acc, float *al, const float *rowa5,
                    const float *rowb)
{
    const struct gsh *s = R->s;
    float qy = R->qy, ddy = g_abs(qy) - s->hby + s->r;
    float d = ddy > 0.f ? ddy + 1e-3f - s->r : ddy - s->r + s->eps2;
    /* the drop shadow, where it reaches (d > -aa): constant along the band too. The offset box's
     * straight part covers these pixels (|qx| <= hbx - r): sd2 is its row's distance, or inside */
    float sh = 0.f;
    if (!s->lens && d > -R->aa) {
        float dys = g_abs(qy - 24.f) - s->hby + s->r;
        float sd2 = dys > 0.f ? dys + 1e-3f - s->r : 0.f;
        sh = .32f * R->alpha * g_exp(-g_max(sd2, 0.f) * (1.f / 48.f)) * g_smooth(-R->aa, R->aa * 2.f, d);
    }
    float ny = qy < 0.f ? -1.f : 1.f;
    float e = g_sat(1.f + d * R->ibez), bend = 1.f - g_sqrt(g_max(1.f - e * e, 0.f));
    float cov = g_sat(.5f - d * R->iaa) * R->alpha;
    int x0 = g_ifloor(R->px0) + i0, x1 = x0 + (i1 - i0);
    const float *f5 = rowa5 + 3 * i0, *fb = rowb + 3 * i0;
    if (bend > 0.f) {
        float oy = ny * bend * R->k13, dy = ny * bend * R->k8;
        if (R->G == 1) {
            gw_row_a5_y(R->py + oy, x0, x1, BF5);
            gw_row_b_y(R->py + oy, x0, x1, BFB);
            gw_row_a_y(R->py + oy + dy, x0, x1, BAP);
            gw_row_a_y(R->py + oy - dy, x0, x1, BAM);
        } else {                    /* the half-resolution grid */
            float sx = R->px0 + (float)(i0 * R->G), dp = (float)R->G;
            gw_row_a5_at(R->py + oy, i1 - i0, sx, dp, BF5);
            gw_row_b_at(R->py + oy, i1 - i0, sx, dp, BFB);
            gw_row_a_at(R->py + oy + dy, i1 - i0, sx, dp, BAP);
            gw_row_a_at(R->py + oy - dy, i1 - i0, sx, dp, BAM);
        }
        f5 = BF5;
        fb = BFB;
    }
    /* shade_tail with n = (0, ny): everything but the frost and the specular is constant */
    float f = ny * R->Ldy, fp = g_max(f, 0.f), fn = g_max(-f, 0.f);
    float add = s->milk * (1.f - bend);
    if (d > -30.f)
        add += g_exp(g_min(d, 0.f) / R->rimk) * (.07f + .6f * fp * fp + .32f * fn * fn) * R->lit;
    float band = 0.f, far = 1.f;
    if (bend > 0.f) {
        band = bend * (.03f + .08f * fp) * R->lit;
        far = 1.f - bend * .14f * fn;
    }
    float kl = (1.f - e) * .82f;
    float32x4_t vomx = vdupq_n_f32(R->omx), vmix = vdupq_n_f32(s->mixf), vvib = vdupq_n_f32(s->vib);
    float32x4_t lr = vdupq_n_f32(.2126f), lg = vdupq_n_f32(.7152f), lb = vdupq_n_f32(.0722f);
    float32x4_t k11 = vdupq_n_f32(1.1f), k014 = vdupq_n_f32(.014f), k42 = vdupq_n_f32(.42f), k13 = vdupq_n_f32(1.3f);
    float32x4_t vtA = vdupq_n_f32(s->tA), vadd = vdupq_n_f32(add + band), vfar = vdupq_n_f32(far);
    float32x4_t vsp = vdupq_n_f32(.045f * R->lit * R->ey), vcov = vdupq_n_f32(cov), vbend = vdupq_n_f32(bend);
    float32x4_t vt[3] = { vdupq_n_f32(s->tr), vdupq_n_f32(s->tg), vdupq_n_f32(s->tb) };
    float32x4_t one = vdupq_n_f32(1.f), k107 = vdupq_n_f32(1.07f), k018 = vdupq_n_f32(.018f), vkl = vdupq_n_f32(kl);
    float32x4_t vsh = vdupq_n_f32(sh), vksh = vdupq_n_f32(1.f - sh);
    const float *ex = s->ex + (R->gx0 - s->ex0);
    int i = i0, dis = bend > 0.f;
    for (; i + 4 <= i1; i += 4) {
        int j = i - i0;
        float32x4x3_t a5 = vld3q_f32(f5 + 3 * j), b = vld3q_f32(fb + 3 * j), A = vld3q_f32(acc + 3 * i);
        float32x4_t l = vld1q_f32(al + i);
        if (sh > 0.f) {
            for (int k = 0; k < 3; k++)
                A.val[k] = vmulq_f32(A.val[k], vksh);
            l = vfmaq_f32(l, vsh, vsubq_f32(one, l));
        }
        float32x4_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = vfmaq_f32(vmulq_f32(a5.val[k], vomx), b.val[k], vmix);
        if (dis) {
            float32x4x3_t ap = vld3q_f32(BAP + 3 * j), am = vld3q_f32(BAM + 3 * j);
            c[0] = vfmaq_f32(c[0], vsubq_f32(ap.val[0], c[0]), vbend);
            c[2] = vfmaq_f32(c[2], vsubq_f32(am.val[2], c[2]), vbend);
        }
        float32x4_t L = vfmaq_f32(vfmaq_f32(vmulq_f32(c[0], lr), c[1], lg), c[2], lb);
        float32x4_t kk = vfmaq_f32(k42, L, k13);
        float32x4_t sp = vmulq_f32(vld1q_f32(ex + i), vsp);
        for (int k = 0; k < 3; k++) {
            float32x4_t r = vfmaq_f32(k014, vfmaq_f32(L, vsubq_f32(c[k], L), vvib), k11);
            r = vfmaq_f32(r, vsubq_f32(vmulq_f32(vt[k], kk), r), vtA);
            r = vfmaq_f32(sp, vaddq_f32(r, vadd), vfar);
            if (s->lens)
                r = vfmaq_f32(r, vsubq_f32(vfmaq_f32(k018, A.val[k], k107), r), vkl);
            A.val[k] = vfmaq_f32(A.val[k], vsubq_f32(r, A.val[k]), vcov);
        }
        vst3q_f32(acc + 3 * i, A);
        if (s->lens && R->mop)
            vst1q_f32(al + i, vsubq_f32(one, vmulq_f32(vsubq_f32(one, l), vfmaq_f32(vsubq_f32(one, vcov), vcov, vmulq_f32(vkl, k107)))));
        else
            vst1q_f32(al + i, vfmaq_f32(l, vcov, vsubq_f32(one, l)));
    }
    if (i < i1)
        px_general(R, i, i1, acc, al, rowa5, rowb);
}

/* A straight side (vertical edge) span: rows where the four normal samples stay on the side's
 * straight part (ddy + 1.5 <= 0) and pixels nearer the side than the top or bottom by more than
 * 3 px (ddx > ddy + 3): there GLASS_FS's normal is (+-1, 0) and d depends on x only, so the
 * refracted samples move along the row: A5, B and A of the row are blended vertically once (VS*)
 * and every sample is one lerp, instead of 4 bilinear samples and 5 SDF evaluations a pixel. */
#define VSN (AW + 8)
static float VS5[3 * VSN], VSA[3 * VSN], VSB[3 * VSN];
static inline void vs_at(const float *V, int t0, int n, float cx, float o[3])
{
    int x0 = g_ifloor(cx);
    float fx = cx - (float)x0;
    int k = x0 - t0;
    if (k < 0) {
        k = 0;
        fx = 0.f;
    } else if (k > n - 2) {
        k = n - 2;
        fx = 1.f;
    }
    o[0] = V[k] + (V[k + 1] - V[k]) * fx;
    o[1] = V[VSN + k] + (V[VSN + k + 1] - V[VSN + k]) * fx;
    o[2] = V[2 * VSN + k] + (V[2 * VSN + k + 1] - V[2 * VSN + k]) * fx;
}
static void px_side(const struct rowc *R, int i0, int i1, float *acc, float *al, const float *rowa5,
                    const float *rowb)
{
    if (i0 >= i1)
        return;
    const struct gsh *s = R->s;
    float qy = R->qy, aa = R->aa, iaa = R->iaa, ibez = R->ibez, alpha = R->alpha, G = (float)R->G;
    /* the samples' x range: the pixels, refracted by up to k13, dispersed by up to k8 */
    float xa = R->px0 + (float)i0 * G - R->k13 - R->k8 - 2.f, xb = R->px0 + (float)(i1 - 1) * G + R->k13 + R->k8 + 2.f;
    int a0 = g_ifloor(xa * .25f - .5f), a1 = g_ifloor(xb * .25f - .5f) + 1;
    int b0 = g_ifloor(xa * .125f - .5f), b1 = g_ifloor(xb * .125f - .5f) + 1;
    if (a1 - a0 + 1 > VSN || b1 - b0 + 1 > VSN) {
        px_general(R, i0, i1, acc, al, rowa5, rowb);
        return;
    }
    gw_vrow_a5(R->py, a0, a1, VS5, VSN);
    gw_vrow_a(R->py, a0, a1, VSA, VSN);
    if (s->mixf > 0.f)
        gw_vrow_b(R->py, b0, b1, VSB, VSN);
    int na = a1 - a0 + 1, nb = b1 - b0 + 1;
    float ddy = g_abs(qy) - s->hby + s->r;
    float qs = qy - 24.f;
    for (int i = i0; i < i1; i++) {
        float px = R->px0 + (float)i * G;
        float qx = px - s->cx;
        float ddx = g_abs(qx) - s->hbx + s->r;
        float d = ddx > 0.f ? ddx + 1e-3f - s->r : g_max(ddx, ddy) - s->r + s->eps2;
        float *A = acc + 3 * i;
        if (!s->lens && d > -aa) {
            float sd2 = sdq(s, qx, qs);
            float sh = .32f * alpha * g_exp(-g_max(sd2, 0.f) * (1.f / 48.f)) * g_smooth(-aa, aa * 2.f, d);
            float k = 1.f - sh;
            A[0] *= k;
            A[1] *= k;
            A[2] *= k;
            al[i] += sh * (1.f - al[i]);
        }
        if (d > aa)
            continue;
        float cov = g_sat(.5f - d * iaa) * alpha;
        int ei = R->gx0 + i - s->ex0;
        float spec = (ei >= 0 && ei < s->exn ? s->ex[ei] : 0.f) * R->ey;
        float col[3], e, bend, nx = 0.f;
        if (d <= R->deep) {
            const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
            col[0] = fa[0] * R->omx + fb[0] * s->mixf;
            col[1] = fa[1] * R->omx + fb[1] * s->mixf;
            col[2] = fa[2] * R->omx + fb[2] * s->mixf;
            e = 0.f;
            bend = 0.f;
        } else {
            nx = qx < 0.f ? -1.f : 1.f;
            e = g_sat(1.f + d * ibez);
            bend = 1.f - g_sqrt(g_max(1.f - e * e, 0.f));
            if (bend > 0.f) {
                float ox = nx * bend * R->k13, x = px + ox, t[3];
                vs_at(VS5, a0, na, x * .25f - .5f, col);
                if (s->mixf > 0.f) {
                    vs_at(VSB, b0, nb, x * .125f - .5f, t);
                    col[0] += (t[0] - col[0]) * s->mixf;
                    col[1] += (t[1] - col[1]) * s->mixf;
                    col[2] += (t[2] - col[2]) * s->mixf;
                }
                float dx = nx * bend * R->k8;
                vs_at(VSA, a0, na, (x + dx) * .25f - .5f, t);
                col[0] += (t[0] - col[0]) * bend;
                vs_at(VSA, a0, na, (x - dx) * .25f - .5f, t);
                col[2] += (t[2] - col[2]) * bend;
            } else {
                const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
                col[0] = fa[0] * R->omx + fb[0] * s->mixf;
                col[1] = fa[1] * R->omx + fb[1] * s->mixf;
                col[2] = fa[2] * R->omx + fb[2] * s->mixf;
            }
        }
        shade_tail(col, d, nx, 0.f, bend, s->tr, s->tg, s->tb, s->tA, s->vib, s->milk, R->lit, R->rimk, R->Ldx, R->Ldy,
                   spec);
        float kl = 0.f;
        if (s->lens) {
            kl = (1.f - e) * .82f;
            col[0] += (A[0] * 1.07f + .018f - col[0]) * kl;
            col[1] += (A[1] * 1.07f + .018f - col[1]) * kl;
            col[2] += (A[2] * 1.07f + .018f - col[2]) * kl;
        }
        A[0] += (col[0] - A[0]) * cov;
        A[1] += (col[1] - A[1]) * cov;
        A[2] += (col[2] - A[2]) * cov;
        al[i] = s->lens && R->mop ? lens_al(al[i], cov, kl) : al[i] + cov * (1.f - al[i]);
    }
}

/* [a, b) per pixel, its straight side part (per pixel: ddx > ddy + 3, on rows with ddy + 1.5 <= 0)
 * through px_side */
static void px_sides(const struct rowc *R, int a, int b, float *acc, float *al, const float *rowa5, const float *rowb)
{
    if (a >= b)
        return;
    const struct gsh *s = R->s;
    float ddy = g_abs(R->qy) - s->hby + s->r;
    if (ddy + 1.5f > 0.f) {
        px_general(R, a, b, acc, al, rowa5, rowb);
        return;
    }
    /* ddx > ddy + 3 + 1e-3: |qx| > hbx - r + ddy + 3.001 */
    float X = s->hbx - s->r + ddy + 3.001f, G = (float)R->G;
    int la = g_ifloor((s->cx - X - R->px0) / G) + 1;   /* left side: i < la ... strictly px < cx - X */
    int lb = (int)__builtin_ceilf((s->cx + X - R->px0) / G);  /* right side: i >= lb */
    /* pixels with px < cx - X or px > cx + X; check the boundary pixels exactly */
    while (la > a && !(R->px0 + (float)(la - 1) * G < s->cx - X))
        la--;
    while (la < b && R->px0 + (float)la * G < s->cx - X)
        la++;
    while (lb < b && !(R->px0 + (float)lb * G > s->cx + X))
        lb++;
    while (lb > a && R->px0 + (float)(lb - 1) * G > s->cx + X)
        lb--;
    int l1 = la < a ? a : la > b ? b : la, r0 = lb < a ? a : lb > b ? b : lb;
    if (l1 > r0)
        l1 = r0;
    px_side(R, a, l1, acc, al, rowa5, rowb);
    px_general(R, l1, r0, acc, al, rowa5, rowb);
    px_side(R, r0, b, acc, al, rowa5, rowb);
}

/* a general segment [a, b): its straight-band part (if any) through px_band, the rest per pixel */
static void px_rim(const struct rowc *R, int a, int b, float *acc, float *al, const float *rowa5, const float *rowb)
{
    if (a >= b)
        return;
    const struct gsh *s = R->s;
    const float h = 1.5f;
    float ddy = g_abs(R->qy) - s->hby + s->r;
    float lim;                       /* ddx <= lim: the straight-band condition */
    if (ddy - h > 0.f)
        lim = -h;
    else if (ddy + h <= 0.f)
        lim = g_min(-h, ddy - 2.f * h - 1e-3f);
    else
        lim = -1e9f;
    if (lim < -1e8f) {
        px_general(R, a, b, acc, al, rowa5, rowb);
        return;
    }
    /* |qx| <= hbx - r + lim (pixel centres px0 + i G) */
    float X = s->hbx - s->r + lim, G = (float)R->G;
    int ba = (int)__builtin_ceilf((s->cx - X - R->px0) / G), bb = g_ifloor((s->cx + X - R->px0) / G) + 1;
    if (ba < a) ba = a;
    if (bb > b) bb = b;
    if (bb - ba < 8) {
        px_sides(R, a, b, acc, al, rowa5, rowb);
        return;
    }
    px_sides(R, a, ba, acc, al, rowa5, rowb);
    px_band(R, ba, bb, acc, al, rowa5, rowb);
    px_sides(R, bb, b, acc, al, rowa5, rowb);
}

/* the first pixel index in [a, b) whose sdq is > th, on a side where it grows with i (dir 1) or
 * with -i (dir -1, then: the last index + 1 whose value is > th) */
static int bound(const struct rowc *R, int a, int b, float th, int dir)
{
    const struct gsh *s = R->s;
    /* invariant (dir 1): f(lo - 1) <= th, f(hi) > th */
    int lo = a, hi = b;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        float d = sdq(s, R->px0 + (float)(m * R->G) - s->cx, R->qy);
        if (dir > 0 ? d > th : d <= th)
            hi = m;
        else
            lo = m + 1;
    }
    return lo;
}

void gs_row(int layer, const struct gs_ctx *c, float py, float px0, int n, float *acc, float *al,
            const float *rowa5, const float *rowb, int s0, int s1)
{
    struct rowc R;
    R.G = GRID[layer];
    R.mop = OPQ[layer];
    R.lit = LIT[layer];
    R.rimk = RIMK[layer];
    R.Ldx = c->Ldx;
    R.Ldy = c->Ldy;
    R.px0 = px0;
    R.gx0 = g_ifloor(px0 / (float)R.G - .5f + .25f);     /* grid index of pixel 0 */
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    if (s1 > NSH[layer])
        s1 = NSH[layer];
    for (int si = s0; si < s1; si++) {
        const struct gsh *s = &SH[layer][si];
        float qy = py - s->cy;
        if (g_abs(qy) > s->hby + 150.f)
            continue;
        float alpha = s->alpha;
        if (s->clip)
            alpha *= fade;
        if (alpha < .002f)
            continue;
        /* pixels with |qx| <= hbx + 110 */
        float xa = s->cx - s->hbx - 110.f, xb = s->cx + s->hbx + 110.f;
        int i0 = (int)__builtin_ceilf((xa - px0) / (float)R.G - 1e-4f), i1 = g_ifloor((xb - px0) / (float)R.G + 1e-4f);
        if (i0 < 0)
            i0 = 0;
        if (i1 > n - 1)
            i1 = n - 1;
        if (i0 > i1)
            continue;
        i1++;                       /* exclusive from here */
        R.s = s;
        R.qy = qy;
        R.py = py;
        R.alpha = alpha;
        R.aa = s->aa;
        R.iaa = 1.f / s->aa;
        R.bez = s->bez;
        R.ibez = s->ibez;
        float dyq = (py - s->spy) * s->sdy;
        R.ey = g_exp(-2.2f * dyq * dyq);
        R.deep = -s->bez - 4.f;
        R.k13 = s->bez * 1.3f * s->s;
        R.k8 = 8.f * s->s;
        R.omx = 1.f - s->mixf;
        /* the row's classes: sdq grows with |qx|, so each threshold is one index per side */
        int ic = (int)__builtin_ceilf((s->cx - px0) / (float)R.G);
        if (ic < i0) ic = i0;
        if (ic > i1) ic = i1;
        float tin = g_min(g_min(R.deep, -30.f), -R.aa - 1e-3f), tout = 2.f * R.aa;
        /* right: [ic, rin) interior, [rin, rout) general, [rout, i1) shadow only */
        int rin = bound(&R, ic, i1, tin, 1), rout = bound(&R, rin, i1, tout, 1);
        /* left: [i0, lout) shadow only, [lout, lin) general, [lin, ic) interior */
        int lout = bound(&R, i0, ic, tout, -1), lin = bound(&R, lout, ic, tin, -1);
        if (!s->lens) {
            px_shadow(&R, i0, lout, acc, al);
            px_shadow(&R, rout, i1, acc, al);
        }
        if (lin == rin) {           /* no interior: one rim segment (a top or bottom band) */
            px_rim(&R, lout, rout, acc, al, rowa5, rowb);
        } else {
            px_rim(&R, lout, lin, acc, al, rowa5, rowb);
            px_interior(&R, lin, rin, acc, al, rowa5, rowb);
            px_rim(&R, rin, rout, acc, al, rowa5, rowb);
        }
    }
}

/* ---- the glass wordmark (uText / uCap / uTxP) ---- */

static struct {
    const uint8_t *hm;
    int hw, hh;
    float fx, fy, fw, fh;
    float tm, tf, ta;
    struct gsh cap;
    int on;
} WD;

int gs_load_height(void)
{
    uint32_t sz;
    const struct sb7_height *h = (const struct sb7_height *)sb7_get(SB7_T_HEIGHT, 0, &sz);
    if (!h || sz < sizeof(*h) + (uint32_t)h->w * h->h)
        return 0;
    WD.hm = (const uint8_t *)(h + 1);
    WD.hw = h->w;
    WD.hh = h->h;
    WD.fx = h->fx;
    WD.fy = h->fy;
    WD.fw = h->fw;
    WD.fh = h->fh;
    return 1;
}

void gs_word_prepare(const struct g7_word *w, const struct gs_ctx *c)
{
    (void)c;
    WD.on = WD.hm && w->ta > .001f;
    WD.tm = w->tm;
    WD.tf = w->tf;
    WD.ta = w->ta;
    struct gsh *s = &WD.cap;
    s->hbx = w->cap[2] * .5f;
    s->hby = w->cap[3] * .5f;
    s->cx = w->cap[0];
    s->cy = w->cap[1];
    sq_params(s, s->hby);
    /* shade(p, d, n, uCap.xy, uCap.zw * .5, 18, ...): the specular lobe of the capsule */
    s->spx = s->cx + c->Ldx * s->hbx * .6f;
    s->spy = s->cy + c->Ldy * s->hby * .6f;
    s->sdx = 1.f / (s->hbx * 1.05f + 50.f);
    s->sdy = 1.f / (s->hby * 1.05f + 50.f);
}

int gs_word_rows(int *y0, int *y1, int *x0, int *x1)
{
    if (!WD.on)
        return 0;
    float ax = g_min(WD.fx, WD.cap.cx - WD.cap.hbx - 8.f), bx = g_max(WD.fx + WD.fw, WD.cap.cx + WD.cap.hbx + 8.f);
    float ay = g_min(WD.fy, WD.cap.cy - WD.cap.hby - 8.f), by = g_max(WD.fy + WD.fh, WD.cap.cy + WD.cap.hby + 8.f);
    *x0 = g_ifloor(ax) - 3;
    *x1 = g_ifloor(bx) + 4;
    *y0 = g_ifloor(ay) - 3;
    *y1 = g_ifloor(by) + 4;
    return 1;
}

static inline float fld(float px, float py)
{
    float qx = (px - WD.fx) / WD.fw, qy = (py - WD.fy) / WD.fh, ft = 0.f;
    if (qx > 0.f && qy > 0.f && qx < 1.f && qy < 1.f) {
        float cx = qx * (float)WD.hw - .5f, cy = qy * (float)WD.hh - .5f;
        int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
        float fx = cx - (float)x0, fy = cy - (float)y0;
        int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 >= WD.hw ? WD.hw - 1 : x0 + 1;
        int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 >= WD.hh ? WD.hh - 1 : y0 + 1;
        const uint8_t *m = WD.hm;
        float a = m[ya * WD.hw + xa] + (m[ya * WD.hw + xb] - m[ya * WD.hw + xa]) * fx;
        float b = m[yb * WD.hw + xa] + (m[yb * WD.hw + xb] - m[yb * WD.hw + xa]) * fx;
        ft = (a + (b - a) * fy) * (1.f / 255.f);
    }
    float fc = 0.f;
    if (WD.tm > 0.f)
        fc = g_sat(.5f - sdq(&WD.cap, px - WD.cap.cx, py - WD.cap.cy) / 14.f);
    return ft + (fc - ft) * WD.tm;
}

void gs_word_row(const struct gs_ctx *c, float py, int x0, int n, float *acc)
{
    gs_word_row_g(c, py, (float)x0 + .5f, 1.f, n, acc);
}
void gs_word_row_g(const struct gs_ctx *c, float py, float px0, float dpx, int n, float *acc)
{
    if (!WD.on)
        return;
    float lit = 1.f - c->soft * .85f, rimk = g_max(2.f, c->K * 1.1f);
    const float bez = 18.f;
    float dyq = (py - WD.cap.spy) * WD.cap.sdy, ey = g_exp(-2.2f * dyq * dyq);
    for (int i = 0; i < n; i++) {
        float px = px0 + (float)i * dpx;
        float f0 = fld(px, py);
        if (f0 <= .02f)
            continue;
        const float e = 2.5f;
        float gx = fld(px + e, py) - fld(px - e, py), gy = fld(px, py + e) - fld(px, py - e);
        float ln = 1.f / g_max(g_sqrt(gx * gx + gy * gy), 1e-5f);
        float nx = -gx * ln, ny = -gy * ln;
        float d = (.5f - f0) * 30.f;
        float cov = g_smooth(.42f, .58f, f0) * WD.ta;
        /* shade(p, d, n, uCap.xy, uCap.zw * .5, 18., vec4(.02, .035, .08, .12), 1.) with SZ = FR = 0 */
        float ee = g_sat(1.f + d / bez), bend = 1.f - g_sqrt(g_max(1.f - ee * ee, 0.f));
        float ox = nx * bend * bez * 1.3f, oy = ny * bend * bez * 1.3f, col[3], t[3];
        frost_at(px + ox, py + oy, g_max(c->soft * .8f, 0.f), col);
        float dx = nx * bend * 8.f, dy = ny * bend * 8.f;
        gw_sample_a(px + ox + dx, py + oy + dy, t);
        col[0] += (t[0] - col[0]) * bend;
        gw_sample_a(px + ox - dx, py + oy - dy, t);
        col[2] += (t[2] - col[2]) * bend;
        float dxq = (px - WD.cap.spx) * WD.cap.sdx, spec = g_exp(-2.2f * dxq * dxq) * ey;
        shade_tail(col, d, nx, ny, bend, .02f, .035f, .08f, .12f, 1.45f, .016f, lit, rimk, c->Ldx, c->Ldy, spec);
        float *A = acc + 3 * i;
        for (int k = 0; k < 3; k++) {
            float g3 = col[k] + (1.f - col[k]) * WD.tf;
            A[k] += (g3 - A[k]) * cov;
        }
    }
}

/* ---- dither and store ---- */

float g_dxt[GW];
void gs_dither_init(void)
{
    for (int x = 0; x < GW; x++)
        g_dxt[x] = g_fract(((float)x + .5f) * 443.897f);
}

void gs_store_row(uint32_t *dst, int y, int x0, int n, const float *acc)
{
    float Y = gs_dither_y(y);
    int i = 0;
    const float32x4_t k = vdupq_n_f32(255.f);
    for (; i + 4 <= n; i += 4) {
        float32x4x3_t a = vld3q_f32(acc + 3 * i);
        float32x4_t h = g_dither4(x0 + i, Y);
        vst1q_u32(dst + i, g_pack4(vfmaq_f32(h, a.val[0], k), vfmaq_f32(h, a.val[1], k), vfmaq_f32(h, a.val[2], k)));
    }
    for (; i < n; i++) {
        float h = g_dither(x0 + i, Y);
        const float *a = acc + 3 * i;
        dst[i] = 0xff000000U | g_q255(a[0] * 255.f + h) << 16 | g_q255(a[1] * 255.f + h) << 8 | g_q255(a[2] * 255.f + h);
    }
}

void gs_over_row(uint32_t *dst, int y, int x0, int n, const float *acc, const float *al)
{
    float Y = gs_dither_y(y);
    int i = 0;
    const float32x4_t k255 = vdupq_n_f32(255.f), zero = vdupq_n_f32(0.f), one = vdupq_n_f32(1.f),
                      half = vdupq_n_f32(.5f);
    for (; i + 4 <= n; i += 4) {
        float32x4_t a = vld1q_f32(al + i);
        if (vmaxvq_f32(a) <= 0.f)
            continue;
        float32x4x3_t c = vld3q_f32(acc + 3 * i);
        float32x4_t h = vfmaq_f32(half, vsubq_f32(g_dither4(x0 + i, Y), half), a), k = vsubq_f32(one, a);
        float32x4_t ka = vmulq_f32(a, k255);
        float32x4_t bg[3];
        g_unpack4(dst + i, bg);
        float32x4_t o[3];
        for (int ch = 0; ch < 3; ch++) {
            float32x4_t cs = vminq_f32(vmaxq_f32(c.val[ch], zero), one);
            o[ch] = vfmaq_f32(vfmaq_f32(h, cs, ka), bg[ch], k);
        }
        uint32x4_t px = g_pack4(o[0], o[1], o[2]), keep = vcleq_f32(a, zero);
        vst1q_u32(dst + i, vbslq_u32(keep, vld1q_u32(dst + i), px));
    }
    for (; i < n; i++) {
        float a = al[i];
        if (a <= 0.f)
            continue;
        float h = (g_dither(x0 + i, Y) - .5f) * a, k = 1.f - a;
        const float *c = acc + 3 * i;
        uint32_t v = dst[i];
        float r = g_sat(c[0]) * a * 255.f + h + (float)CR(v) * k + .5f;
        float g = g_sat(c[1]) * a * 255.f + h + (float)CG(v) * k + .5f;
        float b = g_sat(c[2]) * a * 255.f + h + (float)CB(v) * k + .5f;
        dst[i] = 0xff000000U | g_q255(r) << 16 | g_q255(g) << 8 | g_q255(b);
    }
}
