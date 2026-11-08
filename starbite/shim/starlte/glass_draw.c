/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass: the content items of a layer (glass.h G7_TEXT .. G7_STROKE), drawn over
 * what is below in paint order, with straight-alpha colours blended on sRGB values as the
 * browser does. Also the image / tile cache in G7_CACHE and text measurement.
 *
 * A target is the canvas (opaque) or a transparent premultiplied buffer (the soft copy of the base
 * layer at half size), with a transform target = panel * s + o; every coverage function works in
 * panel px at the inverse-mapped pixel centre, so the same code draws both.
 *
 * Text: glyph pen positions in float px with the font's 1/64 advances and kerning, letter-spacing
 * after every character (CSS), glyph origins rounded to whole pixels at scale 1, bilinear from the
 * atlas when scaled. Tiles: rendered once per (tile, size) from their sb7_tile record (squircle
 * n = 5 as the concept's 72-point polygon, gradient, glyph layer, sheen, rim) into the cache.
 */
#include "glass_int.h"

/* ---- fonts ---- */

struct gfont {
    const struct sb7_font *f;
    const struct sb7_glyph *g;
    const struct sb7_kern *k;
    const uint8_t *bits;
};
static struct gfont FT[SB7_F_COUNT];

static int font_load(unsigned id)
{
    uint32_t sz;
    const struct sb7_font *f = (const struct sb7_font *)sb7_get(SB7_T_FONT, id, &sz);
    if (!f || sz < sizeof(*f))
        return 0;
    if ((uint64_t)f->glyph_off + (uint64_t)f->nglyph * sizeof(struct sb7_glyph) > sz ||
        (uint64_t)f->kern_off + (uint64_t)f->nkern * sizeof(struct sb7_kern) > sz ||
        (uint64_t)f->bits_off + f->bits_size > sz)
        return 0;
    FT[id].f = f;
    FT[id].g = (const struct sb7_glyph *)((const uint8_t *)f + f->glyph_off);
    FT[id].k = (const struct sb7_kern *)((const uint8_t *)f + f->kern_off);
    FT[id].bits = (const uint8_t *)f + f->bits_off;
    return 1;
}
const struct sb7_font *gd_font(unsigned id) { return id < SB7_F_COUNT ? FT[id].f : 0; }

static int glyph_find(const struct gfont *F, uint32_t cp)
{
    int lo = 0, hi = (int)F->f->nglyph - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t c = F->g[m].cp;
        if (c == cp)
            return m;
        if (c < cp)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}
static int32_t kern_find(const struct gfont *F, int l, int r)
{
    int lo = 0, hi = (int)F->f->nkern - 1;
    uint32_t key = (uint32_t)l << 16 | (uint32_t)r;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t k = (uint32_t)F->k[m].left << 16 | F->k[m].right;
        if (k == key)
            return F->k[m].adj64;
        if (k < key)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}
/* next code point of UTF-8 s[*i .. n) */
static uint32_t utf8_next(const char *s, unsigned n, unsigned *i)
{
    const uint8_t *u = (const uint8_t *)s;
    uint32_t c = u[(*i)++];
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more)
        c &= 0x3fU >> more;
    while (more-- && *i < n)
        c = c << 6 | (u[(*i)++] & 0x3fU);
    return c;
}

/* walk a run (two segments, for "text…"): call back with each glyph's pen position */
struct tl {
    const struct gfont *F;
    float pen, trk;                 /* px */
    int prev;
};
static inline int tl_step(struct tl *t, uint32_t cp, float *at)
{
    int g = glyph_find(t->F, cp);
    if (g < 0)
        return -1;
    if (t->prev >= 0)
        t->pen += (float)kern_find(t->F, t->prev, g) * (1.f / 64.f);
    *at = t->pen;
    t->pen += (float)t->F->g[g].adv64 * (1.f / 64.f) + t->trk;
    t->prev = g;
    return g;
}

float g7_text_w(unsigned font, const char *s, unsigned n, float track)
{
    if (font >= SB7_F_COUNT || !FT[font].f)
        return 0.f;
    struct tl t = { &FT[font], 0.f, track * FT[font].f->px, -1 };
    float at;
    for (unsigned i = 0; i < n;)
        tl_step(&t, utf8_next(s, n, &i), &at);
    return t.pen;
}
unsigned g7_text_fit(unsigned font, const char *s, unsigned n, float track, float max_w)
{
    if (font >= SB7_F_COUNT || !FT[font].f)
        return 0;
    struct tl t = { &FT[font], 0.f, track * FT[font].f->px, -1 };
    float at;
    unsigned i = 0, ok = 0;
    while (i < n) {
        struct tl save = t;
        tl_step(&t, utf8_next(s, n, &i), &at);
        if (t.pen > max_w + 1e-3f) {
            t = save;
            break;
        }
        ok = i;
    }
    return ok;
}
void g7_font_metrics(unsigned font, float *ascent, float *descent, float *gap)
{
    const struct sb7_font *f = font < SB7_F_COUNT ? FT[font].f : 0;
    *ascent = f ? (float)f->asc64 / 64.f : 0.f;
    *descent = f ? (float)f->desc64 / 64.f : 0.f;
    *gap = f ? (float)f->gap64 / 64.f : 0.f;
}

/* ---- the cache (G7_CACHE): decoded images and rendered tiles, bump allocated ---- */

#define NCE 192
static struct ce {
    uint16_t kind, id, size, fmt;   /* kind 1 image, 2 tile; size: SB7_TILE_* for tiles */
    uint16_t w, h, nx, ny, nw, nh;
    uint32_t off;
} CE[NCE];
static int nce;
static uint32_t cache_top;

static void cache_reset(void)
{
    nce = 0;
    cache_top = 0;
}
static uint8_t *cache_alloc(uint32_t bytes, uint32_t *off)
{
    bytes = (bytes + 15U) & ~15U;
    if (cache_top + bytes > G7_CACHE_SIZE || nce >= NCE) {
        if (bytes > G7_CACHE_SIZE)
            return 0;
        cache_reset();
    }
    *off = cache_top;
    cache_top += bytes;
    return (uint8_t *)G7_CACHE + *off;
}

static inline uint32_t rd_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* decode an sb7 image's pixels into dst (A8: 1 byte, ARGB: u32 per pixel); 0 if malformed */
static int img_decode(const struct sb7_image *im, uint32_t sz, void *dst)
{
    unsigned bpp = im->format == SB7_ARGB ? 4 : 1;
    uint64_t total = (uint64_t)im->w * im->h;
    const uint8_t *p = (const uint8_t *)(im + 1), *end = p + (sz - sizeof(*im));
    if (im->data_size > sz - sizeof(*im))
        return 0;
    end = p + im->data_size;
    uint8_t *d8 = (uint8_t *)dst;
    uint32_t *d32 = (uint32_t *)dst;
    if (im->enc == SB7_RAW) {
        if ((uint64_t)im->data_size < total * bpp)
            return 0;
        for (uint64_t i = 0; i < total; i++) {
            if (bpp == 4)
                d32[i] = rd_u32(p + 4 * i);
            else
                d8[i] = p[i];
        }
        return 1;
    }
    uint64_t o = 0;
    while (o < total && p + 2 <= end) {
        uint32_t c = rd_u16(p);
        p += 2;
        if (c < 0x8000U) {
            if (p + (uint64_t)c * bpp > end || o + c > total)
                return 0;
            for (uint32_t k = 0; k < c; k++, o++) {
                if (bpp == 4)
                    d32[o] = rd_u32(p + 4 * k);
                else
                    d8[o] = p[k];
            }
            p += c * bpp;
        } else {
            c -= 0x8000U;
            if (p + bpp > end || o + c > total)
                return 0;
            uint32_t v = bpp == 4 ? rd_u32(p) : p[0];
            for (uint32_t k = 0; k < c; k++, o++) {
                if (bpp == 4)
                    d32[o] = v;
                else
                    d8[o] = (uint8_t)v;
            }
            p += bpp;
        }
        if ((uintptr_t)(p - (const uint8_t *)(im + 1)) & 1U)
            p++;
    }
    return o == total;
}

static struct ce *cache_find(unsigned kind, unsigned id, unsigned size)
{
    for (int i = 0; i < nce; i++)
        if (CE[i].kind == kind && CE[i].id == id && CE[i].size == size)
            return &CE[i];
    return 0;
}

static struct ce *image_get(unsigned id)
{
    struct ce *e = cache_find(1, id, 0);
    if (e)
        return e;
    uint32_t sz;
    const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, id, &sz);
    if (!im || sz < sizeof(*im) || !im->w || !im->h)
        return 0;
    unsigned bpp = im->format == SB7_ARGB ? 4 : 1;
    uint32_t off;
    uint8_t *dst = cache_alloc((uint32_t)im->w * im->h * bpp, &off);
    if (!dst || !img_decode(im, sz, dst))
        return 0;
    e = &CE[nce++];
    e->kind = 1;
    e->id = (uint16_t)id;
    e->size = 0;
    e->fmt = im->format;
    e->w = im->w;
    e->h = im->h;
    e->nx = im->nx;
    e->ny = im->ny;
    e->nw = im->nw ? im->nw : im->w;
    e->nh = im->nh ? im->nh : im->h;
    e->off = off;
    return e;
}

int g7_image_size(unsigned id, unsigned *w, unsigned *h)
{
    uint32_t sz;
    const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, id, &sz);
    if (!im || sz < sizeof(*im))
        return 0;
    *w = im->nw ? im->nw : im->w;
    *h = im->nh ? im->nh : im->h;
    return 1;
}

/* ---- tiles ---- */

static const uint16_t tile_px[4] = { 92, 124, 220, 380 };

/* the concept's sqPath(5): 72 segments, coordinates rounded to .01 (toFixed(2)) */
static void sq_poly(float *px, float *py)
{
    for (int i = 0; i <= 72; i++) {
        float a = (float)i / 72.f * 6.28318531f, c = g_cos(a), s = g_sin(a);
        float x = 50.f + 50.f * (c < 0.f ? -1.f : c > 0.f ? 1.f : 0.f) * g_pow(g_abs(c), .4f);
        float y = 50.f + 50.f * (s < 0.f ? -1.f : s > 0.f ? 1.f : 0.f) * g_pow(g_abs(s), .4f);
        px[i] = g_floor(x * 100.f + .5f) / 100.f;
        py[i] = g_floor(y * 100.f + .5f) / 100.f;
    }
}
static float seg_dist(float x, float y, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
    float l = vx * vx + vy * vy, t = l > 0.f ? g_sat((wx * vx + wy * vy) / l) : 0.f;
    float dx = wx - vx * t, dy = wy - vy * t;
    return g_sqrt(dx * dx + dy * dy);
}
static inline uint32_t over_premul(uint32_t d, float r, float g, float b, float a)
{
    /* premultiplied (r, g, b, a in 0..1) over premultiplied d */
    float k = 1.f - a;
    float R = r * 255.f + (float)CR(d) * k, G = g * 255.f + (float)CG(d) * k, B = b * 255.f + (float)CB(d) * k,
          A = a * 255.f + (float)CA(d) * k;
    uint32_t r8 = (uint32_t)g_clamp(R + .5f, 0.f, 255.f), g8 = (uint32_t)g_clamp(G + .5f, 0.f, 255.f),
             b8 = (uint32_t)g_clamp(B + .5f, 0.f, 255.f), a8 = (uint32_t)g_clamp(A + .5f, 0.f, 255.f);
    return a8 << 24 | r8 << 16 | g8 << 8 | b8;
}

static struct ce *tile_get(unsigned tid, unsigned si)
{
    if (si > 3)
        si = 3;
    struct ce *e = cache_find(2, tid, si);
    if (e)
        return e;
    uint32_t sz;
    const struct sb7_tile *tl = (const struct sb7_tile *)sb7_get(SB7_T_TILE, tid, &sz);
    if (!tl || sz < sizeof(*tl))
        return 0;
    unsigned S = tile_px[si];
    uint32_t off;
    uint32_t *px = (uint32_t *)cache_alloc(S * S * 4U, &off);
    if (!px)
        return 0;
    static float pxs[73], pys[73];
    sq_poly(pxs, pys);
    float k = (float)S / 100.f;
    /* 1. squircle coverage: 16 sub-rows a pixel row, exact horizontal span coverage (convex) */
    float *cov = g_row_tmp;             /* S floats */
    for (unsigned y = 0; y < S; y++) {
        for (unsigned x = 0; x < S; x++)
            cov[x] = 0.f;
        for (int sub = 0; sub < 16; sub++) {
            float yy = ((float)y + (sub + .5f) / 16.f) / k;
            float xl = 1e9f, xr = -1e9f;
            for (int i = 0; i < 72; i++) {
                float ay = pys[i], by = pys[i + 1];
                if ((ay <= yy && by > yy) || (by <= yy && ay > yy)) {
                    float xc = pxs[i] + (yy - ay) / (by - ay) * (pxs[i + 1] - pxs[i]);
                    xl = g_min(xl, xc);
                    xr = g_max(xr, xc);
                }
            }
            if (xr <= xl)
                continue;
            xl *= k;
            xr *= k;
            int a = g_ifloor(xl), b = g_ifloor(xr);
            for (int x = a < 0 ? 0 : a; x <= b && x < (int)S; x++) {
                float c = g_min(xr, (float)x + 1.f) - g_max(xl, (float)x);
                if (c > 0.f)
                    cov[x] += c / 16.f;
            }
        }
        /* 2. the gradient c1 -> c2 from (0, 0) to (.75, 1) of the box */
        for (unsigned x = 0; x < S; x++) {
            float u = ((float)x + .5f) / (float)S, v = ((float)y + .5f) / (float)S;
            float t = g_sat((u * .75f + v) / (.5625f + 1.f)), m = g_sat(cov[x]);
            float r = g_mix((float)CR(tl->c1), (float)CR(tl->c2), t) / 255.f;
            float g = g_mix((float)CG(tl->c1), (float)CG(tl->c2), t) / 255.f;
            float b = g_mix((float)CB(tl->c1), (float)CB(tl->c2), t) / 255.f;
            px[y * S + x] = over_premul(0, r * m, g * m, b * m, m);
        }
        /* keep the coverage for the sheen: in the top byte of a scratch row */
        uint8_t *mk = (uint8_t *)P_SCR + (P_SCR_BYTES - 400U * 400U) + y * S;
        for (unsigned x = 0; x < S; x++)
            mk[x] = (uint8_t)(g_sat(cov[x]) * 255.f + .5f);
    }
    /* 3. the glyph layer (TG()), its nominal box on the tile box */
    unsigned gid = tl->glyph[si];
    if (gid) {
        uint32_t gsz;
        const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, gid, &gsz);
        uint32_t *gl = (uint32_t *)P_SCR;
        if (im && gsz >= sizeof(*im) && im->format == SB7_ARGB && (uint64_t)im->w * im->h * 4U <= P_SCR_BYTES - 400U * 400U &&
            img_decode(im, gsz, gl)) {
            float nw = im->nw ? im->nw : im->w, nh = im->nh ? im->nh : im->h;
            float sx = nw / (float)S, sy = nh / (float)S;
            int exact = im->nw == S && im->nh == S;
            for (unsigned y = 0; y < S; y++)
                for (unsigned x = 0; x < S; x++) {
                    uint32_t s;
                    if (exact) {
                        int gx = (int)x + im->nx, gy = (int)y + im->ny;
                        if (gx < 0 || gy < 0 || gx >= im->w || gy >= im->h)
                            continue;
                        s = gl[gy * im->w + gx];
                    } else {
                        float cx = ((float)x + .5f) * sx + im->nx - .5f, cy = ((float)y + .5f) * sy + im->ny - .5f;
                        int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
                        float fx = cx - x0, fy = cy - y0, acc[4] = { 0, 0, 0, 0 };
                        for (int j = 0; j < 4; j++) {
                            int xx = x0 + (j & 1), yy = y0 + (j >> 1);
                            if (xx < 0 || yy < 0 || xx >= im->w || yy >= im->h)
                                continue;
                            float w = ((j & 1) ? fx : 1.f - fx) * ((j >> 1) ? fy : 1.f - fy);
                            uint32_t v = gl[yy * im->w + xx];
                            acc[0] += w * CA(v);
                            acc[1] += w * CR(v);
                            acc[2] += w * CG(v);
                            acc[3] += w * CB(v);
                        }
                        s = (uint32_t)(acc[0] + .5f) << 24 | (uint32_t)(acc[1] + .5f) << 16 | (uint32_t)(acc[2] + .5f) << 8 |
                            (uint32_t)(acc[3] + .5f);
                    }
                    if (CA(s))
                        px[y * S + x] = over_premul(px[y * S + x], CR(s) / 255.f, CG(s) / 255.f, CB(s) / 255.f, CA(s) / 255.f);
                }
        }
    }
    /* 4. the sheen (tsheen: white .28 at 0, white 0 at .45, black .14 at 1; SVG interpolates
     *    unpremultiplied) and 5. the rim, 1.1 units at rgba(255,255,255,.24) on the outline */
    float hw = 1.1f * k * .5f;
    for (unsigned y = 0; y < S; y++) {
        const uint8_t *mk = (const uint8_t *)P_SCR + (P_SCR_BYTES - 400U * 400U) + y * S;
        float v = ((float)y + .5f) / (float)S, c, a;
        if (v <= .45f) {
            c = 1.f;
            a = .28f * (1.f - v / .45f);
        } else {
            float u = (v - .45f) / .55f;
            c = 1.f - u;
            a = .14f * u;
        }
        for (unsigned x = 0; x < S; x++) {
            float m = mk[x] / 255.f;
            uint32_t d = px[y * S + x];
            if (m > 0.f)
                d = over_premul(d, c * a * m, c * a * m, c * a * m, a * m);
            /* the rim: only near the outline */
            float ux = ((float)x + .5f) / k, uy = ((float)y + .5f) / k;
            float ax = g_abs(ux - 50.f) / 50.f, ay = g_abs(uy - 50.f) / 50.f;
            float F = g_pow(g_pow(ax, 5.f) + g_pow(ay, 5.f), .2f);
            if (g_abs(F - 1.f) * 50.f * k < 3.f + hw) {
                float dm = 1e9f;
                for (int i = 0; i < 72; i++)
                    dm = g_min(dm, seg_dist(ux, uy, pxs[i], pys[i], pxs[i + 1], pys[i + 1]));
                float rc = g_sat(hw - dm * k + .5f) * .24f;
                if (rc > 0.f)
                    d = over_premul(d, rc, rc, rc, rc);
            }
            px[y * S + x] = d;
        }
    }
    e = &CE[nce++];
    e->kind = 2;
    e->id = (uint16_t)tid;
    e->size = (uint16_t)si;
    e->fmt = SB7_ARGB;
    e->w = e->h = e->nw = e->nh = (uint16_t)S;
    e->nx = e->ny = 0;
    e->off = off;
    return e;
}

/* ---- blending ---- */

/* straight colour (r, g, b 0..255) at alpha A (0..1) over the target pixel */
static inline void blend(uint32_t *d, uint32_t r, uint32_t g, uint32_t b, float A, int premul)
{
    if (A <= 0.f)
        return;
    uint32_t a = (uint32_t)(g_sat(A) * 255.f + .5f), ia = 255U - a, v = *d;
    uint32_t R = g_div255(r * a + CR(v) * ia), G = g_div255(g * a + CG(v) * ia), B = g_div255(b * a + CB(v) * ia);
    uint32_t AA = premul ? a + g_div255(CA(v) * ia) : 255U;
    *d = AA << 24 | R << 16 | G << 8 | B;
}
/* premultiplied source (0..255 each) times k over the target pixel */
static inline void blend_pm(uint32_t *d, float sr, float sg, float sb, float sa, float k, int premul)
{
    if (k <= 0.f || sa <= 0.f)
        return;
    uint32_t v = *d;
    float ia = 1.f - sa * k * (1.f / 255.f);
    float R = sr * k + CR(v) * ia, G = sg * k + CG(v) * ia, B = sb * k + CB(v) * ia;
    uint32_t r8 = (uint32_t)g_clamp(R + .5f, 0.f, 255.f), g8 = (uint32_t)g_clamp(G + .5f, 0.f, 255.f),
             b8 = (uint32_t)g_clamp(B + .5f, 0.f, 255.f);
    uint32_t a8 = premul ? (uint32_t)g_clamp(sa * k + CA(v) * ia + .5f, 0.f, 255.f) : 255U;
    *d = a8 << 24 | r8 << 16 | g8 << 8 | b8;
}
static inline void saturate(float *r, float *g, float *b, float s, float a)
{
    if (s == 1.f)
        return;
    float R = *r, G = *g, B = *b;
    *r = g_clamp((.213f + .787f * s) * R + (.715f - .715f * s) * G + (.072f - .072f * s) * B, 0.f, a);
    *g = g_clamp((.213f - .213f * s) * R + (.715f + .285f * s) * G + (.072f - .072f * s) * B, 0.f, a);
    *b = g_clamp((.213f - .213f * s) * R + (.715f - .715f * s) * G + (.072f + .928f * s) * B, 0.f, a);
}

/* NEON: n pixels of d (opaque target) under an 8-bit coverage run cv x K / 256 in colour (r, g, b):
 * blend() with the alpha rounded in integer (within one unit of the float path) */
static void blend_a8_run(uint32_t *d, const uint8_t *cv, int n, uint32_t K, uint32_t r, uint32_t g, uint32_t b)
{
    int x = 0;
    const uint8x8_t cr = vdup_n_u8((uint8_t)r), cg = vdup_n_u8((uint8_t)g), cb = vdup_n_u8((uint8_t)b);
    const uint16x8_t vk = vdupq_n_u16((uint16_t)K), r128 = vdupq_n_u16(128);
    for (; x + 8 <= n; x += 8) {
        uint8x8_t c = vld1_u8(cv + x);
        if (!vmaxv_u8(c))
            continue;
        uint8x8_t a = vshrn_n_u16(vmlaq_u16(r128, vmovl_u8(c), vk), 8), ia = vmvn_u8(a);
        uint8x8x4_t v = vld4_u8((const uint8_t *)(d + x));
        const uint8x8_t src[3] = { cb, cg, cr };
        for (int k = 0; k < 3; k++) {
            uint16x8_t t = vaddq_u16(vmlal_u8(vmull_u8(src[k], a), v.val[k], ia), r128);
            v.val[k] = vshrn_n_u16(vaddq_u16(t, vshrq_n_u16(t, 8)), 8);
        }
        v.val[3] = vdup_n_u8(255);
        vst4_u8((uint8_t *)(d + x), v);
    }
    for (; x < n; x++) {
        if (!cv[x])
            continue;
        uint32_t a = (cv[x] * K + 128U) >> 8, ia = 255U - a, v = d[x];
        d[x] = 0xff000000U | g_div255(r * a + CR(v) * ia) << 16 | g_div255(g * a + CG(v) * ia) << 8 | g_div255(b * a + CB(v) * ia);
    }
}
/* NEON: n premultiplied ARGB source pixels x K / 256 over d (opaque target), blend_pm() in integer */
static void blend_pm_run(uint32_t *d, const uint32_t *s, int n, uint32_t K)
{
    int x = 0;
    const uint16x8_t vk = vdupq_n_u16((uint16_t)K), r128 = vdupq_n_u16(128);
    for (; x + 8 <= n; x += 8) {
        uint8x8x4_t p = vld4_u8((const uint8_t *)(s + x));
        if (!vmaxv_u8(p.val[3]))
            continue;
        uint8x8x4_t v = vld4_u8((const uint8_t *)(d + x));
        uint8x8_t sa = K >= 256U ? p.val[3] : vshrn_n_u16(vmlaq_u16(r128, vmovl_u8(p.val[3]), vk), 8), ia = vmvn_u8(sa);
        for (int k = 0; k < 3; k++) {
            uint8x8_t sc = K >= 256U ? p.val[k] : vshrn_n_u16(vmlaq_u16(r128, vmovl_u8(p.val[k]), vk), 8);
            uint16x8_t t = vaddq_u16(vmull_u8(v.val[k], ia), r128);
            v.val[k] = vqadd_u8(sc, vshrn_n_u16(vaddq_u16(t, vshrq_n_u16(t, 8)), 8));
        }
        v.val[3] = vdup_n_u8(255);
        vst4_u8((uint8_t *)(d + x), v);
    }
    for (; x < n; x++) {
        uint32_t q = s[x];
        if (!CA(q))
            continue;
        uint32_t sa = K >= 256U ? CA(q) : (CA(q) * K + 128U) >> 8, ia = 255U - sa, v = d[x];
        uint32_t sr = K >= 256U ? CR(q) : (CR(q) * K + 128U) >> 8, sg = K >= 256U ? CG(q) : (CG(q) * K + 128U) >> 8,
                 sb = K >= 256U ? CB(q) : (CB(q) * K + 128U) >> 8;
        uint32_t R = sr + g_div255(CR(v) * ia), G = sg + g_div255(CG(v) * ia), B = sb + g_div255(CB(v) * ia);
        d[x] = 0xff000000U | (R > 255U ? 255U : R) << 16 | (G > 255U ? 255U : G) << 8 | (B > 255U ? 255U : B);
    }
}

/* ---- per item drawing context ---- */

struct dc {
    const struct gd_target *t;
    int x0, y0, x1, y1;             /* target px region for this item */
    float a;                        /* item opacity */
    float is;                       /* 1 / s */
    int fade;                       /* G7_I_FADE */
    const struct g7_clip *clip;     /* rounded clip (r > 0) or 0 */
};
static inline float pan_x(const struct dc *c, int tx) { return ((float)tx + .5f - c->t->ox) * c->is; }
static inline float pan_y(const struct dc *c, int ty) { return ((float)ty + .5f - c->t->oy) * c->is; }
static inline float fade_y(float y) { return g_sat((y - 170.f) / 76.f) * g_sat((2620.f - y) / 140.f); }
/* rounded rect SDF (circular corners), box centre c, half size h */
static inline float rr_sd(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    r = g_min(r, g_min(hx, hy));
    float qx = g_abs(px - cx) - (hx - r), qy = g_abs(py - cy) - (hy - r);
    float ox = g_max(qx, 0.f), oy = g_max(qy, 0.f);
    return g_sqrt(ox * ox + oy * oy) + g_min(g_max(qx, qy), 0.f) - r;
}
/* the clip and fade factor at a target pixel row / pixel */
static inline float clip_k(const struct dc *c, float px, float py)
{
    float k = c->fade ? fade_y(py) : 1.f;
    if (c->clip) {
        const struct g7_clip *q = c->clip;
        float hx = (q->x1 - q->x0) * .5f, hy = (q->y1 - q->y0) * .5f;
        float d = rr_sd(px, py, q->x0 + hx, q->y0 + hy, hx, hy, q->r);
        k *= g_sat(.5f - d * c->t->s);
    }
    return k;
}

/* ---- text ---- */

static void draw_glyph(const struct dc *c, const struct gfont *F, int gi, float ox, float oy, float sc,
                       const struct g7_item *it)
{
    const struct sb7_glyph *g = &F->g[gi];
    if (!g->w || !g->h)
        return;
    const uint8_t *bm = F->bits + g->off;
    const struct g7_text *tx = &it->u.text;
    uint32_t col = it->color;
    float ca = (float)CA(col) / 255.f * c->a;
    int grad = (it->flags & G7_I_GRAD) != 0;
    float ystop = tx->gy0 + tx->gstop * (tx->gy1 - tx->gy0);
    const struct gd_target *t = c->t;
    float k = sc * t->s;               /* glyph px -> target px */
    if (g_abs(k - 1.f) < 1e-3f) {
        /* whole-pixel origin */
        int gx = g_ifloor(ox * t->s + t->ox + .5f) + g->bx, gy = g_ifloor(oy * t->s + t->oy + .5f) + g->by;
        int ya = gy < c->y0 ? c->y0 : gy, yb = gy + g->h > c->y1 ? c->y1 : gy + g->h;
        int xa = gx < c->x0 ? c->x0 : gx, xb = gx + g->w > c->x1 ? c->x1 : gx + g->w;
        for (int y = ya; y < yb; y++) {
            const uint8_t *src = bm + (y - gy) * g->w;
            uint32_t *d = t->px + (long)y * t->stride;
            float py = pan_y(c, y);
            uint32_t r = CR(col), gg = CG(col), b = CB(col);
            if (grad && py > ystop) {
                float u = g_sat((py - ystop) / g_max(tx->gy1 - ystop, 1e-3f));
                r = (uint32_t)(g_mix((float)CR(col), (float)CR(tx->color2), u) + .5f);
                gg = (uint32_t)(g_mix((float)CG(col), (float)CG(tx->color2), u) + .5f);
                b = (uint32_t)(g_mix((float)CB(col), (float)CB(tx->color2), u) + .5f);
            }
            if (!c->fade && !c->clip && !t->premul) {
                if (xb > xa)
                    blend_a8_run(d + xa, src + (xa - gx), xb - xa, (uint32_t)(g_sat(ca) * 256.f + .5f), r, gg, b);
                continue;
            }
            for (int x = xa; x < xb; x++) {
                uint32_t cv = src[x - gx];
                if (!cv)
                    continue;
                float A = (float)cv / 255.f * ca;
                if (c->fade || c->clip)
                    A *= clip_k(c, pan_x(c, x), py);
                blend(d + x, r, gg, b, A, t->premul);
            }
        }
        return;
    }
    /* scaled: bilinear from the atlas; glyph box in target px */
    float tx0 = (ox + g->bx * sc) * t->s + t->ox, ty0 = (oy + g->by * sc) * t->s + t->oy;
    int xa = g_ifloor(tx0), ya = g_ifloor(ty0), xb = (int)__builtin_ceilf(tx0 + g->w * k) + 1,
        yb = (int)__builtin_ceilf(ty0 + g->h * k) + 1;
    if (xa < c->x0) xa = c->x0;
    if (ya < c->y0) ya = c->y0;
    if (xb > c->x1) xb = c->x1;
    if (yb > c->y1) yb = c->y1;
    float ik = 1.f / k;
    for (int y = ya; y < yb; y++) {
        float v = ((float)y + .5f - ty0) * ik - .5f;
        int v0 = g_ifloor(v);
        float fv = v - (float)v0;
        if (v0 < -1 || v0 >= g->h)
            continue;
        uint32_t *d = t->px + (long)y * t->stride;
        float py = pan_y(c, y);
        uint32_t r = CR(col), gg = CG(col), b = CB(col);
        if (grad && py > ystop) {
            float u = g_sat((py - ystop) / g_max(tx->gy1 - ystop, 1e-3f));
            r = (uint32_t)(g_mix((float)CR(col), (float)CR(tx->color2), u) + .5f);
            gg = (uint32_t)(g_mix((float)CG(col), (float)CG(tx->color2), u) + .5f);
            b = (uint32_t)(g_mix((float)CB(col), (float)CB(tx->color2), u) + .5f);
        }
        for (int x = xa; x < xb; x++) {
            float u = ((float)x + .5f - tx0) * ik - .5f;
            int u0 = g_ifloor(u);
            float fu = u - (float)u0;
            if (u0 < -1 || u0 >= g->w)
                continue;
            float s00 = (u0 >= 0 && v0 >= 0) ? bm[v0 * g->w + u0] : 0.f;
            float s01 = (u0 + 1 < g->w && v0 >= 0) ? bm[v0 * g->w + u0 + 1] : 0.f;
            float s10 = (u0 >= 0 && v0 + 1 < g->h) ? bm[(v0 + 1) * g->w + u0] : 0.f;
            float s11 = (u0 + 1 < g->w && v0 + 1 < g->h) ? bm[(v0 + 1) * g->w + u0 + 1] : 0.f;
            float cv = (s00 + (s01 - s00) * fu) * (1.f - fv) + (s10 + (s11 - s10) * fu) * fv;
            if (cv <= .5f)
                continue;
            float A = cv / 255.f * ca;
            if (c->fade || c->clip)
                A *= clip_k(c, pan_x(c, x), py);
            blend(d + x, r, gg, b, A, t->premul);
        }
    }
}

static const char ELL[] = "\xe2\x80\xa6";       /* … */

static void draw_text(const struct dc *c, const struct g7_item *it)
{
    const struct g7_text *tx = &it->u.text;
    if (tx->font >= SB7_F_COUNT || !FT[tx->font].f || !tx->s)
        return;
    const struct gfont *F = &FT[tx->font];
    unsigned n = tx->n, cut = n;
    int ell = 0;
    float sc = tx->scale > 0.f ? tx->scale : 1.f;
    if ((it->flags & G7_I_ELLIP) && tx->max_w > 0.f && g7_text_w(tx->font, tx->s, n, tx->track) > tx->max_w + .01f) {
        float ew = g7_text_w(tx->font, ELL, 3, tx->track);
        cut = g7_text_fit(tx->font, tx->s, n, tx->track, tx->max_w - ew);
        while (cut && tx->s[cut - 1] == ' ')
            cut--;
        ell = 1;
    }
    struct tl t = { F, 0.f, tx->track * F->f->px, -1 };
    float at;
    for (unsigned i = 0; i < cut;) {
        int g = tl_step(&t, utf8_next(tx->s, cut, &i), &at);
        if (g >= 0)
            draw_glyph(c, F, g, tx->x + at * sc, tx->y, sc, it);
    }
    if (ell)
        for (unsigned i = 0; i < 3;) {
            int g = tl_step(&t, utf8_next(ELL, 3, &i), &at);
            if (g >= 0)
                draw_glyph(c, F, g, tx->x + at * sc, tx->y, sc, it);
        }
}

/* ---- images and tiles: a bitmap with its nominal box placed on (x, y, w, h) ---- */

static void draw_bitmap(const struct dc *c, const struct ce *e, float x, float y, float w, float h, uint32_t color,
                        float sat)
{
    const struct gd_target *t = c->t;
    const uint8_t *base = (const uint8_t *)G7_CACHE + e->off;
    int argb = e->fmt == SB7_ARGB;
    float sx = w / (float)e->nw, sy = h / (float)e->nh;     /* bitmap px -> panel px */
    float ks = sx * t->s, kt = sy * t->s;
    float ca = argb ? c->a : (float)CA(color) / 255.f * c->a;
    /* bitmap box in target px */
    float bx0 = (x - e->nx * sx) * t->s + t->ox, by0 = (y - e->ny * sy) * t->s + t->oy;
    if (g_abs(ks - 1.f) < 1e-3f && g_abs(kt - 1.f) < 1e-3f) {
        int gx = g_ifloor(bx0 + .5f), gy = g_ifloor(by0 + .5f);
        int ya = gy < c->y0 ? c->y0 : gy, yb = gy + e->h > c->y1 ? c->y1 : gy + e->h;
        int xa = gx < c->x0 ? c->x0 : gx, xb = gx + e->w > c->x1 ? c->x1 : gx + e->w;
        int fast = !c->fade && !c->clip && !t->premul && (!argb || sat == 1.f) && xb > xa;
        uint32_t K = (uint32_t)(g_sat(ca) * 256.f + .5f);
        for (int yy = ya; yy < yb; yy++) {
            uint32_t *d = t->px + (long)yy * t->stride;
            if (fast) {
                if (argb)
                    blend_pm_run(d + xa, (const uint32_t *)base + (yy - gy) * e->w + (xa - gx), xb - xa, K);
                else
                    blend_a8_run(d + xa, base + (yy - gy) * e->w + (xa - gx), xb - xa, K, CR(color), CG(color), CB(color));
                continue;
            }
            float py = pan_y(c, yy);
            for (int xx = xa; xx < xb; xx++) {
                float k = ca;
                if (c->fade || c->clip)
                    k *= clip_k(c, pan_x(c, xx), py);
                if (argb) {
                    uint32_t s = ((const uint32_t *)base)[(yy - gy) * e->w + (xx - gx)];
                    if (!CA(s))
                        continue;
                    float r = CR(s), g = CG(s), b = CB(s);
                    saturate(&r, &g, &b, sat, (float)CA(s));
                    blend_pm(d + xx, r, g, b, (float)CA(s), k, t->premul);
                } else {
                    uint32_t cv = base[(yy - gy) * e->w + (xx - gx)];
                    if (cv)
                        blend(d + xx, CR(color), CG(color), CB(color), (float)cv / 255.f * k, t->premul);
                }
            }
        }
        return;
    }
    int xa = g_ifloor(bx0), ya = g_ifloor(by0), xb = (int)__builtin_ceilf(bx0 + e->w * ks) + 1,
        yb = (int)__builtin_ceilf(by0 + e->h * kt) + 1;
    if (xa < c->x0) xa = c->x0;
    if (ya < c->y0) ya = c->y0;
    if (xb > c->x1) xb = c->x1;
    if (yb > c->y1) yb = c->y1;
    float iks = 1.f / ks, ikt = 1.f / kt;
    for (int yy = ya; yy < yb; yy++) {
        float v = ((float)yy + .5f - by0) * ikt - .5f;
        int v0 = g_ifloor(v);
        float fv = v - (float)v0;
        if (v0 < -1 || v0 >= e->h)
            continue;
        uint32_t *d = t->px + (long)yy * t->stride;
        float py = pan_y(c, yy);
        for (int xx = xa; xx < xb; xx++) {
            float u = ((float)xx + .5f - bx0) * iks - .5f;
            int u0 = g_ifloor(u);
            float fu = u - (float)u0;
            if (u0 < -1 || u0 >= e->w)
                continue;
            float k = ca;
            if (c->fade || c->clip)
                k *= clip_k(c, pan_x(c, xx), py);
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < 4; j++) {
                int ux = u0 + (j & 1), vy = v0 + (j >> 1);
                if (ux < 0 || vy < 0 || ux >= e->w || vy >= e->h)
                    continue;
                float wgt = ((j & 1) ? fu : 1.f - fu) * ((j >> 1) ? fv : 1.f - fv);
                if (argb) {
                    uint32_t s = ((const uint32_t *)base)[vy * e->w + ux];
                    acc[0] += wgt * CR(s);
                    acc[1] += wgt * CG(s);
                    acc[2] += wgt * CB(s);
                    acc[3] += wgt * CA(s);
                } else
                    acc[3] += wgt * base[vy * e->w + ux];
            }
            if (acc[3] <= .25f)
                continue;
            if (argb) {
                saturate(&acc[0], &acc[1], &acc[2], sat, acc[3]);
                blend_pm(d + xx, acc[0], acc[1], acc[2], acc[3], k, t->premul);
            } else
                blend(d + xx, CR(color), CG(color), CB(color), acc[3] / 255.f * k, t->premul);
        }
    }
}

/* ---- rects, shadows, arcs, strokes: a coverage function over the item's region ---- */

static void draw_rect(const struct dc *c, const struct g7_item *it)
{
    const struct g7_rect *q = &it->u.rect;
    float hx = q->w * .5f, hy = q->h * .5f, cx = q->x + hx, cy = q->y + hy, r = g_min(q->r, g_min(hx, hy));
    float ca = (float)CA(it->color) / 255.f * c->a, s = c->t->s;
    float ring = q->ring;
    int dash = ring > 0.f && (it->flags & G7_I_DASH);
    /* dashes: Chrome's dashed border, dash = gap = 2 x width, fitted to the circumference */
    float circ = 6.28318531f * g_max(r - ring * .5f, 1.f);
    float nd = g_max(2.f, g_floor(circ / (ring * 4.f) + .5f));
    /* a square-cornered filled rect: where a pixel centre is half a pixel inside, the coverage is 1
     * and every pixel takes the same blend (the full-panel black of the end screens, bars) */
    int fx0 = 1, fx1 = 0, fy0 = 1, fy1 = 0;
    uint32_t fa = 0;
    if (r <= 0.f && ring <= 0.f && !c->fade && !c->clip && !c->t->premul && ca > 0.f) {
        /* target px tx with pan_x(tx) in [q->x + .5 / s, q->x + q->w - .5 / s] */
        float is = c->is, ox = c->t->ox, oy = c->t->oy;
        fx0 = (int)__builtin_ceilf((q->x + .5f * is) * s + ox - .5f);
        fx1 = g_ifloor((q->x + q->w - .5f * is) * s + ox - .5f) + 1;
        fy0 = (int)__builtin_ceilf((q->y + .5f * is) * s + oy - .5f);
        fy1 = g_ifloor((q->y + q->h - .5f * is) * s + oy - .5f) + 1;
        fx0 = fx0 < c->x0 ? c->x0 : fx0;
        fx1 = fx1 > c->x1 ? c->x1 : fx1;
        fy0 = fy0 < c->y0 ? c->y0 : fy0;
        fy1 = fy1 > c->y1 ? c->y1 : fy1;
        fa = (uint32_t)(g_sat(ca) * 255.f + .5f);
    }
    const uint8x8_t vfa = vdup_n_u8((uint8_t)fa), via = vdup_n_u8((uint8_t)(255U - fa));
    const uint8x8_t vr = vdup_n_u8((uint8_t)CR(it->color)), vg = vdup_n_u8((uint8_t)CG(it->color)),
                    vb = vdup_n_u8((uint8_t)CB(it->color));
    for (int y = c->y0; y < c->y1; y++) {
        float py = pan_y(c, y);
        uint32_t *d = c->t->px + (long)y * c->t->stride;
        float fk = c->fade ? fade_y(py) : 1.f;
        int ra = c->x1, rb = c->x1;
        if (y >= fy0 && y < fy1 && fx0 < fx1) {
            ra = fx0;
            rb = fx1;
            /* blend(): (c a + v (255 - a)) / 255 rounded, 8 pixels a step */
            int x = ra;
            for (; x + 8 <= rb; x += 8) {
                uint8x8x4_t v = vld4_u8((const uint8_t *)(d + x));
                const uint8x8_t src[3] = { vb, vg, vr };
                for (int k = 0; k < 3; k++) {
                    uint16x8_t t = vaddq_u16(vmlal_u8(vmull_u8(src[k], vfa), v.val[k], via), vdupq_n_u16(128));
                    v.val[k] = vshrn_n_u16(vaddq_u16(t, vshrq_n_u16(t, 8)), 8);
                }
                v.val[3] = vdup_n_u8(255);
                vst4_u8((uint8_t *)(d + x), v);
            }
            for (; x < rb; x++)
                blend(d + x, CR(it->color), CG(it->color), CB(it->color), ca, 0);
        }
        for (int x = c->x0; x < c->x1; x++) {
            if (x == ra) {
                x = rb - 1;
                continue;
            }
            float px = pan_x(c, x);
            float dd = rr_sd(px, py, cx, cy, hx, hy, r);
            float cov = g_sat(.5f - dd * s);
            if (cov <= 0.f)
                continue;
            if (ring > 0.f) {
                float di = rr_sd(px, py, cx, cy, hx - ring, hy - ring, g_max(r - ring, 0.f));
                cov -= g_sat(.5f - di * s);
                if (dash) {
                    float ang = g_atan2(py - cy, px - cx) / 6.28318531f + .5f;  /* 0..1 */
                    float ph = g_fract(ang * nd), ed = (g_min(ph, 1.f - ph) - .25f) * circ / nd;  /* > 0 inside a dash */
                    cov *= g_sat(.5f + ed * s);
                }
                if (cov <= 0.f)
                    continue;
            }
            float A = cov * ca * fk;
            if (c->clip)
                A *= clip_k(c, px, py) / (c->fade ? g_max(fk, 1e-6f) : 1.f);
            blend(d + x, CR(it->color), CG(it->color), CB(it->color), A, c->t->premul);
        }
    }
}

/* erf from a table (g_erf at 4096 points over [-4, 4], linear between: within 2e-6 of it) */
#define ERF_N 4096
static float ERF_T[ERF_N + 2];
static int erf_ok;
static void erf_init(void)
{
    for (int i = 0; i <= ERF_N + 1; i++)
        ERF_T[i] = g_erf(-4.f + 8.f * (float)i / (float)ERF_N);
    erf_ok = 1;
}
static inline float erf_t(float u)
{
    float t = (u + 4.f) * ((float)ERF_N / 8.f);
    if (t <= 0.f)
        return -1.f;
    if (t >= (float)ERF_N)
        return 1.f;
    int i = (int)t;
    float f = t - (float)i;
    return ERF_T[i] + (ERF_T[i + 1] - ERF_T[i]) * f;
}

/* box-shadow of a rounded rect (Gaussian, sigma = blur / 2): the vertical integral sampled at
 * 6 points, the horizontal one exact through erf (as GPU box-shadow renderers do) */
static void draw_shadow(const struct dc *c, const struct g7_item *it)
{
    const struct g7_boxshadow *q = &it->u.shadow;
    float sp = q->spread, x0 = q->x - sp + q->dx, y0 = q->y - sp + q->dy, w = q->w + 2 * sp, h = q->h + 2 * sp;
    float r = g_max(q->r + sp, 0.f), sig = q->blur * .5f;
    float hx = w * .5f, hy = h * .5f, cx = x0 + hx, cy = y0 + hy;
    r = g_min(r, g_min(hx, hy));
    float ca = (float)CA(it->color) / 255.f * c->a;
    float k = .70710678f / g_max(sig, 1e-3f);
    for (int y = c->y0; y < c->y1; y++) {
        float py = pan_y(c, y) - cy;
        uint32_t *d = c->t->px + (long)y * c->t->stride;
        /* the row's six samples: their weight and half width (independent of x) */
        float wgt[6], cu[6];
        if (sig >= .3f) {
            float lo = py - hy, hi = py + hy;
            float st = g_clamp(-3.f * sig, lo, hi), en = g_clamp(3.f * sig, lo, hi);
            float step = (en - st) / 6.f, yy = st + step * .5f;
            for (int i = 0; i < 6; i++, yy += step) {
                float ly = py - yy;
                float delta = g_min(hy - r - g_abs(ly), 0.f);
                cu[i] = (hx - r + g_sqrt(g_max(0.f, r * r - delta * delta))) * k;
                wgt[i] = .5f * g_exp(-(yy * yy) / (2.f * sig * sig)) * step * (.39894228f / sig);
            }
        }
        for (int x = c->x0; x < c->x1; x++) {
            float px = pan_x(c, x) - cx, v;
            if (sig < .3f) {
                v = g_sat(.5f - rr_sd(px, py, 0.f, 0.f, hx, hy, r) * c->t->s);
            } else {
                float u = px * k;
                v = 0.f;
                for (int i = 0; i < 6; i++)
                    v += wgt[i] * (erf_t(u + cu[i]) - erf_t(u - cu[i]));
            }
            if (v <= .001f)
                continue;
            float A = v * ca;
            if (c->fade || c->clip)
                A *= clip_k(c, px + cx, py + cy);
            blend(d + x, CR(it->color), CG(it->color), CB(it->color), A, c->t->premul);
        }
    }
}

static void draw_arc(const struct dc *c, const struct g7_item *it)
{
    const struct g7_arc *q = &it->u.arc;
    float ca = (float)CA(it->color) / 255.f * c->a, hw = q->width * .5f;
    float a0 = q->a0 * (3.14159265f / 180.f), sw = q->sweep * (3.14159265f / 180.f);
    int full = q->sweep >= 359.99f;
    float e0x = q->cx + q->r * g_sin(a0), e0y = q->cy - q->r * g_cos(a0);
    float e1x = q->cx + q->r * g_sin(a0 + sw), e1y = q->cy - q->r * g_cos(a0 + sw);
    if (q->sweep <= 0.f)
        return;
    for (int y = c->y0; y < c->y1; y++) {
        float py = pan_y(c, y);
        uint32_t *d = c->t->px + (long)y * c->t->stride;
        for (int x = c->x0; x < c->x1; x++) {
            float px = pan_x(c, x), dx = px - q->cx, dy = py - q->cy;
            float rho = g_sqrt(dx * dx + dy * dy), dist;
            if (full)
                dist = g_abs(rho - q->r);
            else {
                float ang = g_atan2(dx, -dy);                  /* clockwise from 12 o'clock */
                float rel = ang - a0;
                rel -= 6.28318531f * g_floor(rel / 6.28318531f);
                if (rel <= sw)
                    dist = g_abs(rho - q->r);
                else {
                    float d0x = px - e0x, d0y = py - e0y, d1x = px - e1x, d1y = py - e1y;
                    dist = g_sqrt(g_min(d0x * d0x + d0y * d0y, d1x * d1x + d1y * d1y));
                }
            }
            float cov = g_sat((hw - dist) * c->t->s + .5f);
            if (cov <= 0.f)
                continue;
            float A = cov * ca;
            if (c->fade || c->clip)
                A *= clip_k(c, px, py);
            blend(d + x, CR(it->color), CG(it->color), CB(it->color), A, c->t->premul);
        }
    }
}

static void draw_stroke(const struct dc *c, const struct g7_item *it)
{
    const struct g7_stroke *q = &it->u.stroke;
    float ca = (float)CA(it->color) / 255.f * c->a, hw = q->width * .5f;
    int n = q->n < 2 ? 2 : q->n > 4 ? 4 : q->n;
    for (int y = c->y0; y < c->y1; y++) {
        float py = pan_y(c, y);
        uint32_t *d = c->t->px + (long)y * c->t->stride;
        for (int x = c->x0; x < c->x1; x++) {
            float px = pan_x(c, x), dist = 1e9f;
            for (int i = 0; i + 1 < n; i++)
                dist = g_min(dist, seg_dist(px, py, q->x[i], q->y[i], q->x[i + 1], q->y[i + 1]));
            float cov = g_sat((hw - dist) * c->t->s + .5f);
            if (cov <= 0.f)
                continue;
            float A = cov * ca;
            if (c->fade || c->clip)
                A *= clip_k(c, px, py);
            blend(d + x, CR(it->color), CG(it->color), CB(it->color), A, c->t->premul);
        }
    }
}

/* ---- bounds (panel px) ---- */

static inline void bset(int16_t b[4], float x0, float y0, float x1, float y1)
{
    b[0] = (int16_t)g_clamp(g_floor(x0), -1.f, (float)GW);
    b[1] = (int16_t)g_clamp(g_floor(y0), -1.f, (float)GH);
    b[2] = (int16_t)g_clamp(__builtin_ceilf(x1), -1.f, (float)GW);
    b[3] = (int16_t)g_clamp(__builtin_ceilf(y1), -1.f, (float)GH);
}

int gd_bounds(const struct g7_layer *l, const struct g7_item *it, int16_t b[4])
{
    if (it->a <= 0.f)
        return 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    switch (it->kind) {
    case G7_TEXT: {
        const struct g7_text *t = &it->u.text;
        if (t->font >= SB7_F_COUNT || !FT[t->font].f || !t->n)
            return 0;
        const struct sb7_font *f = FT[t->font].f;
        float sc = t->scale > 0.f ? t->scale : 1.f, px = f->px;
        float w = g7_text_w(t->font, t->s, t->n, t->track);
        if ((it->flags & G7_I_ELLIP) && t->max_w > 0.f && w > t->max_w)
            w = t->max_w;
        x0 = t->x - .3f * px * sc;
        x1 = t->x + (w + .3f * px) * sc;
        y0 = t->y - ((float)f->asc64 / 64.f + .3f * px) * sc;
        y1 = t->y + ((float)f->desc64 / 64.f + .3f * px) * sc;
        break;
    }
    case G7_IMAGE: {
        const struct g7_image *m = &it->u.image;
        unsigned w, h;
        uint32_t sz;
        const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, m->id, &sz);
        if (!im || !g7_image_size(m->id, &w, &h))
            return 0;
        float sx = m->w / (float)w, sy = m->h / (float)h;
        x0 = m->x - im->nx * sx - 1.f;
        y0 = m->y - im->ny * sy - 1.f;
        x1 = x0 + im->w * sx + 2.f;
        y1 = y0 + im->h * sy + 2.f;
        break;
    }
    case G7_TILE:
        x0 = it->u.tile.x - 1.f;
        y0 = it->u.tile.y - 1.f;
        x1 = it->u.tile.x + it->u.tile.w + 1.f;
        y1 = it->u.tile.y + it->u.tile.w + 1.f;
        break;
    case G7_RECT:
        x0 = it->u.rect.x - 1.f;
        y0 = it->u.rect.y - 1.f;
        x1 = it->u.rect.x + it->u.rect.w + 1.f;
        y1 = it->u.rect.y + it->u.rect.h + 1.f;
        break;
    case G7_SHADOW: {
        const struct g7_boxshadow *q = &it->u.shadow;
        float m = q->blur * 1.5f + g_abs(q->spread) + 2.f;
        x0 = q->x + q->dx - m;
        y0 = q->y + q->dy - m;
        x1 = q->x + q->dx + q->w + m;
        y1 = q->y + q->dy + q->h + m;
        break;
    }
    case G7_ARC: {
        const struct g7_arc *q = &it->u.arc;
        float m = q->r + q->width + 1.f;
        x0 = q->cx - m;
        y0 = q->cy - m;
        x1 = q->cx + m;
        y1 = q->cy + m;
        break;
    }
    case G7_STROKE: {
        const struct g7_stroke *q = &it->u.stroke;
        x0 = y0 = 1e9f;
        x1 = y1 = -1e9f;
        for (int i = 0; i < q->n && i < 4; i++) {
            x0 = g_min(x0, q->x[i]);
            x1 = g_max(x1, q->x[i]);
            y0 = g_min(y0, q->y[i]);
            y1 = g_max(y1, q->y[i]);
        }
        float m = q->width + 1.f;
        x0 -= m;
        y0 -= m;
        x1 += m;
        y1 += m;
        break;
    }
    default:
        return 0;
    }
    if (it->clip < l->nclip) {
        const struct g7_clip *c = &l->clip[it->clip];
        x0 = g_max(x0, c->x0);
        y0 = g_max(y0, c->y0);
        x1 = g_min(x1, c->x1);
        y1 = g_min(y1, c->y1);
    }
    if (it->flags & G7_I_FADE) {
        y0 = g_max(y0, 170.f);
        y1 = g_min(y1, 2620.f);
    }
    bset(b, x0, y0, x1, y1);
    if (b[0] < 0) b[0] = 0;
    if (b[1] < 0) b[1] = 0;
    return b[2] > b[0] && b[3] > b[1];
}

/* ---- the layer's items over the target region ---- */

void gd_items(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t)
{
    gd_items_range(l, bounds, t, 0, l->nitem);
}
static void draw_item(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t, unsigned i)
{
    {
        const struct g7_item *it = &l->item[i];
        const int16_t *b = bounds[i];
        if (b[2] <= b[0] || it->a <= 0.f)
            return;
        struct dc c;
        c.t = t;
        c.a = it->a;
        c.is = 1.f / t->s;
        c.fade = (it->flags & G7_I_FADE) != 0;
        c.clip = 0;
        /* the item's region in target px */
        int x0 = g_ifloor(b[0] * t->s + t->ox), y0 = g_ifloor(b[1] * t->s + t->oy);
        int x1 = (int)__builtin_ceilf(b[2] * t->s + t->ox), y1 = (int)__builtin_ceilf(b[3] * t->s + t->oy);
        c.x0 = x0 > t->x0 ? x0 : t->x0;
        c.y0 = y0 > t->y0 ? y0 : t->y0;
        c.x1 = x1 < t->x1 ? x1 : t->x1;
        c.y1 = y1 < t->y1 ? y1 : t->y1;
        if (c.x0 >= c.x1 || c.y0 >= c.y1)
            return;
        if (it->clip && it->clip < l->nclip && l->clip[it->clip].r > 0.f)
            c.clip = &l->clip[it->clip];
        switch (it->kind) {
        case G7_TEXT:
            draw_text(&c, it);
            break;
        case G7_IMAGE: {
            const struct ce *e = image_get(it->u.image.id);
            if (e)
                draw_bitmap(&c, e, it->u.image.x, it->u.image.y, it->u.image.w, it->u.image.h, it->color,
                            it->u.image.sat > 0.f ? it->u.image.sat : 1.f);
            break;
        }
        case G7_TILE: {
            const struct ce *e = tile_get(it->u.tile.tile, it->u.tile.size);
            if (e)
                draw_bitmap(&c, e, it->u.tile.x, it->u.tile.y, it->u.tile.w, it->u.tile.w, 0xffffffffU,
                            it->u.tile.sat > 0.f ? it->u.tile.sat : 1.f);
            break;
        }
        case G7_RECT:
            draw_rect(&c, it);
            break;
        case G7_SHADOW:
            draw_shadow(&c, it);
            break;
        case G7_ARC:
            draw_arc(&c, it);
            break;
        case G7_STROKE:
            draw_stroke(&c, it);
            break;
        }
    }
}
void gd_items_range(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t, unsigned i0,
                    unsigned i1)
{
    if (i1 > l->nitem)
        i1 = l->nitem;
    for (unsigned i = i0; i < i1; i++)
        draw_item(l, bounds, t, i);
}
void gd_items_list(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t,
                   const uint16_t *idx, unsigned n)
{
    for (unsigned k = 0; k < n; k++)
        if (idx[k] < l->nitem)
            draw_item(l, bounds, t, idx[k]);
}

int gd_init(void)
{
    cache_reset();
    if (!erf_ok)
        erf_init();
    int ok = 1;
    for (unsigned id = 1; id < SB7_F_COUNT; id++) {
        FT[id].f = 0;
        if (!font_load(id))
            ok = 0;
    }
    return ok;
}
